# Focus sweep sharpness on averaged 8x8 blocks of RAW10 high bytes (robust to noise in dark frames): tools/sunfish-sharpness2.py DIR (fA.raw, stride 2528)
import sys,glob,numpy as np
W,H,S=2016,1512,2528
out={}
for f in sorted(glob.glob(sys.argv[1]+'/f*.raw'),key=lambda p:int(p.split('/f')[-1][:-4])):
    b=np.fromfile(f,dtype=np.uint8)
    # hi bytes of RAW10 packed: drop every 5th byte
    rows=b[:S*H].reshape(H,S)[:,:2520]
    hi=rows[:,np.arange(2520)%5!=4].astype(float)  # 2016 px, 8-bit
    # downsample 8x8 block mean (kills Bayer + noise)
    h,w=hi.shape; a=hi[:h//8*8,:w//8*8].reshape(h//8,8,w//8,8).mean((1,3))
    gx=np.diff(a,axis=1);gy=np.diff(a,axis=0)
    c=a[a.shape[0]//4:3*a.shape[0]//4,a.shape[1]//4:3*a.shape[1]//4]
    gxc=np.diff(c,axis=1);gyc=np.diff(c,axis=0)
    out[f]=a
    print(f.split('/')[-1],'mean %.2f min %.1f max %.1f  grad energy all %.4f centre %.4f'%(a.mean(),a.min(),a.max(),(gx**2).mean()+(gy**2).mean(),(gxc**2).mean()+(gyc**2).mean()))
k=list(out)
base=out[k[0]]
for f in k[1:]:
    print('diff vs first',f.split('/')[-1],'mean abs %.3f'%np.abs(out[f]-base).mean())
