// SPDX-License-Identifier: MIT
//! 0116: the bridge's Rust global allocator, for the render thread's per-draw Vecs. Small requests come from per-thread
//! free lists (one lock-free pop or push), everything else from System. Off: M2V_FAST_ALLOC=0, read once before the
//! first allocation. Host tests: tools/test-metal2-alloc.sh.
//!
//! The Layout alone decides where a block belongs, so blocks have no header and may be freed on any thread; they join
//! the freeing thread's list. Blocks never go back to System: a class holds on to its peak use, as any size-class
//! allocator does.
//!
//! Bounds: a thread caches at most 8 KB per class (excess moves to the locked global list), and only 16 threads have a
//! cache at a time, so at most 16 * 96 KB sits in caches whatever happens to threads. A pthread key's destructor hands
//! the cache to the next thread at thread exit. If it never runs, that slot alone is lost, and a thread without a slot
//! uses the global list: slower, still correct.
//!
//! It uses a raw pthread key, not thread_local!: nothing here allocates or runs lazily, so it can't recurse.

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::UnsafeCell;
use std::ffi::c_void;
use std::ptr::{self, null_mut};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

const CLASS_SIZES: [usize; CLASSES] = [16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024];
const CLASSES: usize = 12;
/// Largest request served here; a larger one, or one aligned past `MAX_ALIGN`, goes to System.
const MAX_SMALL: usize = 1024;
/// Every class size is a multiple of this and chunks are aligned to it, so every block is.
const MAX_ALIGN: usize = 16;
const CHUNK: usize = 256 * 1024;
const SLOTS: usize = 16;
/// Blocks moved between a slot and the global list at a time, as bytes; a slot holds at most twice this per class.
const BATCH_BYTES: usize = 4096;

/// The class of a request, indexed by its size in 16-byte units, rounded up.
static CLASS_OF: [u8; MAX_SMALL / 16 + 1] = {
    let mut table = [0; MAX_SMALL / 16 + 1];
    let (mut units, mut class) = (0, 0);
    while units < table.len() {
        if units * 16 > CLASS_SIZES[class] {
            class += 1;
        }
        table[units] = class as u8;
        units += 1;
    }
    table
};

/// Blocks in one transfer: at least 4, so that the largest classes don't take the lock on every other call.
const fn batch(class: usize) -> usize {
    let blocks = BATCH_BYTES / CLASS_SIZES[class];
    if blocks < 4 { 4 } else { blocks }
}

/// A free block holds the next free block's address.
struct Free {
    next: *mut Free,
}

struct Lists {
    head: [*mut Free; CLASSES],
    count: [u32; CLASSES],
}

/// One thread's cache. On its own cache lines: the owners of neighbouring slots run on other cores.
#[repr(align(64))]
struct Slot {
    owned: AtomicBool,
    lists: UnsafeCell<Lists>,
}

// SAFETY: `lists` is touched only by the thread that won `owned`.
unsafe impl Sync for Slot {}

static SLOT_POOL: [Slot; SLOTS] = [const {
    Slot {
        owned: AtomicBool::new(false),
        lists: UnsafeCell::new(Lists { head: [null_mut(); CLASSES], count: [0; CLASSES] }),
    }
}; SLOTS];

struct GlobalLists {
    head: [*mut Free; CLASSES],
    /// The unused part of the newest chunk.
    bump: *mut u8,
    end: *mut u8,
    chunks: usize,
}

/// The slow path's shared state, behind a static pthread mutex (std's Mutex may allocate on some targets).
struct Global {
    lock: UnsafeCell<libc::pthread_mutex_t>,
    lists: UnsafeCell<GlobalLists>,
}

// SAFETY: `lists` is touched only while holding `lock`.
unsafe impl Sync for Global {}

static GLOBAL: Global = Global {
    lock: UnsafeCell::new(libc::PTHREAD_MUTEX_INITIALIZER),
    lists: UnsafeCell::new(GlobalLists { head: [null_mut(); CLASSES], bump: null_mut(), end: null_mut(), chunks: 0 }),
};

/// Runs `f` under the global lock. `f` may call System (malloc never comes back here).
fn locked<R>(f: impl FnOnce(&mut GlobalLists) -> R) -> R {
    // SAFETY: a static, initialized mutex; the lock makes the reference exclusive.
    unsafe {
        libc::pthread_mutex_lock(GLOBAL.lock.get());
        let result = f(&mut *GLOBAL.lists.get());
        libc::pthread_mutex_unlock(GLOBAL.lock.get());
        result
    }
}

impl GlobalLists {
    /// A block of `class`, reused or carved; null when out of memory.
    fn take(&mut self, class: usize) -> *mut Free {
        let block = self.head[class];
        if !block.is_null() {
            // SAFETY: a block on a free list is free and holds its link.
            self.head[class] = unsafe { (*block).next };
            return block;
        }
        let size = CLASS_SIZES[class];
        if (self.end as usize) - (self.bump as usize) < size {
            // The old chunk's tail (under 1 KB) is left unused.
            // SAFETY: the layout has a non-zero size and a valid alignment.
            let chunk = unsafe { System.alloc(Layout::from_size_align_unchecked(CHUNK, MAX_ALIGN)) };
            if chunk.is_null() {
                return null_mut();
            }
            self.bump = chunk;
            // SAFETY: one past the end of the chunk.
            self.end = unsafe { chunk.add(CHUNK) };
            self.chunks += 1;
        }
        let block = self.bump;
        // SAFETY: `size` bytes remain in the chunk.
        self.bump = unsafe { block.add(size) };
        block.cast()
    }

    /// # Safety
    /// `block` is a free block of `class`, owned by the caller.
    unsafe fn put(&mut self, class: usize, block: *mut Free) {
        // SAFETY: per the contract.
        unsafe { (*block).next = self.head[class] };
        self.head[class] = block;
    }
}

/// 0 = not decided yet, 1 = off, otherwise the pthread key + 2.
static STATE: AtomicUsize = AtomicUsize::new(0);

/// Decides once, atomically, so that every block is freed the way it was allocated.
#[cold]
fn decide() -> usize {
    // SAFETY: a NUL-terminated name; getenv doesn't allocate.
    let off = unsafe {
        let value = libc::getenv(c"M2V_FAST_ALLOC".as_ptr());
        !value.is_null() && *value as u8 == b'0' && *value.add(1) == 0
    };
    let mut key: libc::pthread_key_t = 0;
    // SAFETY: `key` is writable and `release_slot` fits the destructor signature.
    let state = if off || unsafe { libc::pthread_key_create(&mut key, Some(release_slot)) } != 0 {
        1
    } else {
        key as usize + 2
    };
    match STATE.compare_exchange(0, state, Ordering::AcqRel, Ordering::Acquire) {
        Ok(_) => state,
        Err(winner) => {
            if state > 1 {
                // SAFETY: our own key, which nobody has seen.
                unsafe { libc::pthread_key_delete(key) };
            }
            winner
        }
    }
}

/// The pthread key, or None when the allocator is off.
#[inline(always)]
fn key() -> Option<libc::pthread_key_t> {
    let mut state = STATE.load(Ordering::Acquire);
    if state == 0 {
        state = decide();
    }
    if state == 1 { None } else { Some((state - 2) as libc::pthread_key_t) }
}

/// Thread exit: the slot and the blocks in it go to the next thread that claims it.
unsafe extern "C" fn release_slot(slot: *mut c_void) {
    // SAFETY: the key only ever holds a pointer into SLOT_POOL.
    unsafe { (*slot.cast::<Slot>()).owned.store(false, Ordering::Release) };
}

/// Gives this thread a free slot; null when all are taken (tried again on the next call, which is on the slow path).
#[cold]
fn claim_slot(key: libc::pthread_key_t) -> *mut Slot {
    for slot in &SLOT_POOL {
        if !slot.owned.load(Ordering::Relaxed) && !slot.owned.swap(true, Ordering::Acquire) {
            let slot = ptr::from_ref(slot).cast_mut();
            // SAFETY: a key from pthread_key_create.
            if unsafe { libc::pthread_setspecific(key, slot.cast()) } != 0 {
                // SAFETY: we own the slot and it's not registered.
                unsafe { release_slot(slot.cast()) };
                return null_mut();
            }
            return slot;
        }
    }
    null_mut()
}

#[inline(always)]
fn my_slot(key: libc::pthread_key_t) -> *mut Slot {
    // SAFETY: a key from pthread_key_create.
    let slot = unsafe { libc::pthread_getspecific(key) }.cast::<Slot>();
    if slot.is_null() { claim_slot(key) } else { slot }
}

#[inline(always)]
fn is_small(size: usize, align: usize) -> bool {
    size <= MAX_SMALL && align <= MAX_ALIGN
}

#[inline(always)]
fn class_of(size: usize) -> usize {
    CLASS_OF[(size + 15) >> 4] as usize
}

/// An empty list: one batch from the global list under one lock. Returns one block, the rest go on the list.
#[cold]
#[inline(never)]
fn refill(lists: &mut Lists, class: usize) -> *mut Free {
    locked(|global| {
        let first = global.take(class);
        if !first.is_null() {
            for _ in 1..batch(class) {
                let block = global.take(class);
                if block.is_null() {
                    break;
                }
                // SAFETY: a free block, now ours.
                unsafe { (*block).next = lists.head[class] };
                lists.head[class] = block;
                lists.count[class] += 1;
            }
        }
        first
    })
}

/// A full list: the newest batch stays (still in the cache), the older blocks move to the global list.
#[cold]
#[inline(never)]
fn flush(lists: &mut Lists, class: usize) {
    let kept = batch(class);
    let mut last_kept = lists.head[class];
    for _ in 1..kept {
        // SAFETY: the list holds `count` > `kept` blocks.
        last_kept = unsafe { (*last_kept).next };
    }
    // SAFETY: `last_kept` is on the list, and more blocks follow it.
    let first = unsafe { (*last_kept).next };
    let mut last = first;
    // SAFETY: the list's links; the last block's is null.
    while let Some(next) = unsafe { (*last).next.as_mut() } {
        last = next;
    }
    // SAFETY: `last_kept` is on the list.
    unsafe { (*last_kept).next = null_mut() };
    lists.count[class] = kept as u32;
    locked(|global| {
        // SAFETY: the chain first..=last is now ours; it's spliced onto the list.
        unsafe { (*last).next = global.head[class] };
        global.head[class] = first;
    });
}

/// The allocator; installed in NativeMetalBridge.rs.
pub struct FastAlloc;

impl FastAlloc {
    #[inline(always)]
    unsafe fn small_alloc(key: libc::pthread_key_t, class: usize) -> *mut u8 {
        let slot = my_slot(key);
        if slot.is_null() {
            return locked(|global| global.take(class)).cast();
        }
        // SAFETY: this thread owns the slot.
        let lists = unsafe { &mut *(*slot).lists.get() };
        let block = lists.head[class];
        if block.is_null() {
            return refill(lists, class).cast();
        }
        // SAFETY: a block on a free list is free and holds its link.
        lists.head[class] = unsafe { (*block).next };
        lists.count[class] -= 1;
        block.cast()
    }

    #[inline(always)]
    unsafe fn small_free(key: libc::pthread_key_t, class: usize, block: *mut u8) {
        let block = block.cast::<Free>();
        let slot = my_slot(key);
        if slot.is_null() {
            // SAFETY: the caller gave up the block.
            locked(|global| unsafe { global.put(class, block) });
            return;
        }
        // SAFETY: this thread owns the slot; the caller gave up the block.
        let lists = unsafe { &mut *(*slot).lists.get() };
        unsafe { (*block).next = lists.head[class] };
        lists.head[class] = block;
        lists.count[class] += 1;
        if lists.count[class] as usize > 2 * batch(class) {
            flush(lists, class);
        }
    }
}

// SAFETY: blocks come from chunks no one else uses, are at least as large as the Layout asks and aligned to 16, and are
// handed out once until freed; the Layout picks the same path in alloc and dealloc, and STATE never changes once set.
unsafe impl GlobalAlloc for FastAlloc {
    #[inline]
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if is_small(layout.size(), layout.align()) {
            if let Some(key) = key() {
                // SAFETY: a valid key.
                return unsafe { Self::small_alloc(key, class_of(layout.size())) };
            }
        }
        // SAFETY: the caller's layout.
        unsafe { System.alloc(layout) }
    }

    #[inline]
    unsafe fn dealloc(&self, block: *mut u8, layout: Layout) {
        if is_small(layout.size(), layout.align()) {
            if let Some(key) = key() {
                // SAFETY: the caller gives up `block`, a block of this class.
                return unsafe { Self::small_free(key, class_of(layout.size()), block) };
            }
        }
        // SAFETY: the caller's block: not small, or the allocator is off, so System's.
        unsafe { System.dealloc(block, layout) }
    }

    #[inline]
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        if is_small(layout.size(), layout.align()) && key().is_some() {
            // SAFETY: the caller's layout; on success `size` bytes are ours.
            unsafe {
                let block = self.alloc(layout);
                if !block.is_null() {
                    ptr::write_bytes(block, 0, layout.size());
                }
                return block;
            }
        }
        // SAFETY: the caller's layout.
        unsafe { System.alloc_zeroed(layout) }
    }

    unsafe fn realloc(&self, block: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        let (old_small, new_small) = (is_small(layout.size(), layout.align()), is_small(new_size, layout.align()));
        if key().is_none() || (!old_small && !new_small) {
            // SAFETY: the caller's arguments; the block is System's.
            return unsafe { System.realloc(block, layout, new_size) };
        }
        if old_small && new_small && class_of(layout.size()) == class_of(new_size) {
            return block;
        }
        // SAFETY: the caller promises new_size is valid for this alignment. Between a class and System, or two classes.
        unsafe {
            let new_layout = Layout::from_size_align_unchecked(new_size, layout.align());
            let new_block = self.alloc(new_layout);
            if !new_block.is_null() {
                ptr::copy_nonoverlapping(block, new_block, layout.size().min(new_size));
                self.dealloc(block, layout);
            }
            new_block
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{mpsc, Arc, Barrier};
    use std::thread;

    // The harness itself and every std call in these tests run on the allocator too.
    #[global_allocator]
    static TESTED: FastAlloc = FastAlloc;

    fn on() -> bool {
        key().is_some()
    }

    fn layout(size: usize, align: usize) -> Layout {
        Layout::from_size_align(size, align).unwrap()
    }

    fn slots_in_use() -> usize {
        SLOT_POOL.iter().filter(|slot| slot.owned.load(Ordering::Acquire)).count()
    }

    #[test]
    fn off_switch_matches_the_environment() {
        assert_eq!(on(), std::env::var("M2V_FAST_ALLOC").map_or(true, |v| v != "0"));
    }

    #[test]
    fn classes_fit_every_small_size() {
        for size in 0..=MAX_SMALL {
            let class = class_of(size);
            assert!(CLASS_SIZES[class] >= size, "{size}");
            assert_eq!(CLASS_SIZES[class] % MAX_ALIGN, 0);
            assert!(class == 0 || CLASS_SIZES[class - 1] < size, "{size} could use a smaller class");
        }
        assert!(CLASS_SIZES.windows(2).all(|pair| pair[0] < pair[1]));
        let cached: usize = (0..CLASSES).map(|c| 2 * batch(c) * CLASS_SIZES[c]).sum();
        assert!(cached <= 96 * 1024, "{cached}");
    }

    #[test]
    fn same_thread_free_is_reused() {
        if !on() {
            return;
        }
        unsafe {
            let a = TESTED.alloc(layout(40, 8));
            TESTED.dealloc(a, layout(40, 8));
            // Same class, different size.
            let b = TESTED.alloc(layout(48, 16));
            assert_eq!(a, b);
            TESTED.dealloc(b, layout(48, 16));
        }
    }

    #[test]
    fn freed_on_another_thread_joins_that_threads_list() {
        let block = unsafe { TESTED.alloc(layout(200, 8)) } as usize;
        assert_ne!(block, 0);
        unsafe { ptr::write_bytes(block as *mut u8, 0x5a, 200) };
        let same = thread::spawn(move || unsafe {
            TESTED.dealloc(block as *mut u8, layout(200, 8));
            let again = TESTED.alloc(layout(200, 8));
            TESTED.dealloc(again, layout(200, 8));
            again as usize == block
        })
        .join()
        .unwrap();
        assert!(same || !on()); // off: System may or may not reuse it
    }

    #[test]
    fn alignment_is_honoured() {
        let aligns = [1, 2, 4, 8, 16, 32, 64, 128, 4096, 65536];
        let sizes = [1, 7, 8, 15, 16, 17, 33, 100, 768, 769, 1024, 1025, 5000];
        for align in aligns {
            for size in sizes {
                unsafe {
                    let block = TESTED.alloc(layout(size, align));
                    assert!(!block.is_null());
                    assert_eq!(block as usize % align, 0, "{size} {align}");
                    ptr::write_bytes(block, 0xa5, size);
                    TESTED.dealloc(block, layout(size, align));
                }
            }
        }
    }

    #[test]
    fn realloc_keeps_contents_across_every_boundary() {
        for align in [1, 16, 64] {
            for (from, to) in [(1, 16), (16, 17), (10, 1000), (1000, 1024), (1024, 1025), (1000, 5000), (5000, 9000),
                (9000, 5000), (5000, 1024), (1025, 1024), (1024, 17), (500, 490), (500, 3), (3, 1)] {
                unsafe {
                    let block = TESTED.alloc(layout(from, align));
                    for i in 0..from {
                        *block.add(i) = i as u8;
                    }
                    let grown = TESTED.realloc(block, layout(from, align), to);
                    assert!(!grown.is_null());
                    assert_eq!(grown as usize % align, 0);
                    for i in 0..from.min(to) {
                        assert_eq!(*grown.add(i), i as u8, "{from}->{to} align {align} byte {i}");
                    }
                    ptr::write_bytes(grown, 0xff, to);
                    TESTED.dealloc(grown, layout(to, align));
                }
            }
        }
    }

    #[test]
    fn realloc_within_a_class_keeps_the_block() {
        if !on() {
            return;
        }
        unsafe {
            let block = TESTED.alloc(layout(600, 8));
            assert_eq!(TESTED.realloc(block, layout(600, 8), 768), block);
            TESTED.dealloc(block, layout(768, 8));
        }
    }

    #[test]
    fn alloc_zeroed_clears_a_dirty_block() {
        for size in [1, 16, 40, 1024, 1025, 70000] {
            unsafe {
                let dirty = TESTED.alloc(layout(size, 8));
                ptr::write_bytes(dirty, 0xee, size);
                TESTED.dealloc(dirty, layout(size, 8));
                let zero = TESTED.alloc_zeroed(layout(size, 8));
                assert!(std::slice::from_raw_parts(zero, size).iter().all(|byte| *byte == 0), "{size}");
                TESTED.dealloc(zero, layout(size, 8));
            }
        }
    }

    #[test]
    fn a_thread_cache_stays_under_its_cap() {
        if !on() {
            return;
        }
        for class in 0..CLASSES {
            let size = CLASS_SIZES[class];
            let blocks: Vec<usize> =
                (0..4 * batch(class) + 3).map(|_| unsafe { TESTED.alloc(layout(size, 8)) } as usize).collect();
            let mut unique = blocks.clone();
            unique.sort();
            unique.dedup();
            assert_eq!(unique.len(), blocks.len(), "a block handed out twice");
            for block in blocks {
                unsafe { TESTED.dealloc(block as *mut u8, layout(size, 8)) };
            }
            let slot = my_slot(key().unwrap());
            let count = unsafe { (*(*slot).lists.get()).count[class] } as usize;
            assert!(count <= 2 * batch(class), "class {class}: {count}");
        }
    }

    #[test]
    fn more_threads_than_slots() {
        let threads = 4 * SLOTS;
        let barrier = Arc::new(Barrier::new(threads));
        let handles: Vec<_> = (0..threads)
            .map(|t| {
                let barrier = barrier.clone();
                thread::spawn(move || {
                    let mut held: Vec<Vec<u8>> = (0..50).map(|i| vec![t as u8; 1 + i * 20]).collect();
                    barrier.wait(); // all alive together: more than SLOTS use the global list
                    for i in 0..5000 {
                        let slot = i % held.len();
                        assert!(held[slot].iter().all(|byte| *byte == t as u8));
                        held[slot] = vec![t as u8; 1 + (i * 37) % 1500];
                    }
                })
            })
            .collect();
        for handle in handles {
            handle.join().unwrap();
        }
    }

    #[test]
    fn exited_threads_give_back_their_slots() {
        let before = slots_in_use();
        let chunks = locked(|global| global.chunks);
        for _ in 0..2000 {
            thread::spawn(|| drop(vec![0u8; 100])).join().unwrap();
        }
        assert!(slots_in_use() <= before + 1, "{before} -> {}", slots_in_use());
        // Thread churn doesn't grow the heap: exited threads' blocks are reused. (Other work may take one chunk.)
        assert!(locked(|global| global.chunks) <= chunks + 1);
    }

    #[test]
    fn blocks_passed_between_threads_keep_their_contents() {
        let (to_worker, from_main) = mpsc::channel::<(u8, Vec<u8>)>();
        let (to_main, from_worker) = mpsc::channel::<(u8, Vec<u8>)>();
        let worker = thread::spawn(move || {
            for (tag, mut data) in from_main {
                assert!(data.iter().all(|byte| *byte == tag));
                data.fill(tag.wrapping_add(1));
                data.resize(data.len() + 17, tag.wrapping_add(1));
                to_main.send((tag.wrapping_add(1), data)).unwrap();
            }
        });
        for i in 0..100_000usize {
            let tag = i as u8;
            to_worker.send((tag, vec![tag; 1 + i % 1400])).unwrap();
            if i % 8 == 7 {
                for _ in 0..8 {
                    let (tag, data) = from_worker.recv().unwrap();
                    assert!(data.iter().all(|byte| *byte == tag));
                }
            }
        }
        drop(to_worker);
        worker.join().unwrap();
    }
}
