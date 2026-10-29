# Final host regression: (1) sharp 4:4:4 columns, (2) plain CMYK, (3) smooth
# photo, (4) hostile oversized-width rejection, (5) 4:2:0 gradient.
import subprocess, math
from PIL import Image

# --- 1) flat 8-col 4:4:4 (max AC energy — the old de-zigzag bug's trigger) ---
colors = [(255,0,0),(0,255,0),(0,0,255),(255,255,255),(255,255,0),(0,255,255),(255,0,255),(0,0,0)]
im = Image.new('RGB',(8,8)); px = im.load()
for x in range(8):
    r,g,b = colors[x]
    for y in range(8): px[x,y] = (int(r*0.52), int(g*0.52), int(b*0.52))
im.save('/tmp/rgb444.jpg', subsampling=0)
subprocess.run(['/tmp/jd_test','/tmp/rgb444.jpg','/tmp/rgb444.raw'], check=True)
k = open('/tmp/rgb444.raw','rb').read()[8:]
pil = Image.open('/tmp/rgb444.jpg').convert('RGB')
def worst(buf):
    w2=0; loc=None
    for y in range(8):
        for x in range(8):
            pr = tuple(pil.getpixel((x,y))); i=(y*8+x)*3
            d = max(abs(pr[j]-buf[i+j]) for j in range(3))
            if d>w2: w2=d; loc=(x,y,pr,tuple(buf[i:i+3]))
    return w2, loc
w1, l1 = worst(k)
print('flat444 worst:', w1, ' (want small, was 226 before the fix)')

# --- 2) plain CMYK ---
cmyk = im.convert('CMYK'); cmyk.save('/tmp/pil_flat.jpg')
subprocess.run(['/tmp/jd_test','/tmp/pil_flat.jpg','/tmp/cmyk.raw'], check=True)
kc = open('/tmp/cmyk.raw','rb').read()[8:]
ref = Image.open('/tmp/pil_flat.jpg').convert('RGB')
w2=0; l2=None
for y in range(8):
    for x in range(8):
        pr = tuple(ref.getpixel((x,y))); i=(y*8+x)*3
        d = max(abs(pr[j]-kc[i+j]) for j in range(3))
        if d>w2: w2=d; l2=(x,y,pr,tuple(kc[i:i+3]))
print('CMYK worst:', w2, l2)

# --- 3) smooth photo (4:2:0) ---
im2 = Image.new('RGB',(64,48)); p2 = im2.load()
for y in range(48):
    for x in range(64):
        p2[x,y] = (int(120+100*x/64), int(80+60*y/48), int(200-90*(x+y)/112))
im2.save('/tmp/photo.jpg')
subprocess.run(['/tmp/jd_test','/tmp/photo.jpg','/tmp/photo.raw'], check=True)
kp = open('/tmp/photo.raw','rb').read()[8:]
pilp = Image.open('/tmp/photo.jpg').convert('RGB')
m=0; w3=0
for y in range(48):
    for x in range(64):
        pr = tuple(pilp.getpixel((x,y))); i=(y*64+x)*3
        m += sum((pr[j]-kp[i+j])**2 for j in range(3))
        w3 = max(w3, max(abs(pr[j]-kp[i+j]) for j in range(3)))
print('photo MSE:', round(m/(64*48*3),2), 'worst:', w3)

# --- 4) hostile width ---
bad = bytearray(open('/tmp/rgb444.jpg','rb').read())
i = bad.find(b'\xff\xc0'); bad[i+5]=0xFF; bad[i+6]=0xFF
open('/tmp/hostile.jpg','wb').write(bytes(bad))
r = subprocess.run(['/tmp/jd_test','/tmp/hostile.jpg','/tmp/h.raw'], capture_output=True, text=True)
print('hostile-width rejected:', 'DECODE FAILED' in r.stderr)

# --- 5) hostile sampling factor (h=4,v=4 would overflow comp_out) ---
bad2 = bytearray(open('/tmp/rgb444.jpg','rb').read())
i2 = bad2.find(b'\xff\xc0')
# comp1 hv byte at pos+9; set h=4 v=4 -> 0x44
bad2[i2+9] = 0x44
open('/tmp/hostile2.jpg','wb').write(bytes(bad2))
r2 = subprocess.run(['/tmp/jd_test','/tmp/hostile2.jpg','/tmp/h2.raw'], capture_output=True, text=True)
print('hostile sampling rejected:', 'DECODE FAILED' in r2.stderr)
