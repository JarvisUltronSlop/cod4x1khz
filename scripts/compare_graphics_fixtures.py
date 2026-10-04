"""Lossless pixel comparison and local-window SSIM for small native fixtures.

This establishes primitive/material parity only, not stock-map image acceptance.
Uses the standard 7x7 uniform-window SSIM with sample covariance, RGB separately,
8-bit data range, K1=.01, K2=.03 and no border padding.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

def ppm(path):
    magic,size,maximum,data=path.read_bytes().split(b'\n',3)
    if magic!=b'P6' or maximum!=b'255':
        raise ValueError('Expected native lossless RGB8 PPM')
    width,height=map(int,size.split())
    if len(data)!=width*height*3 or width*height>1024*1024:
        raise ValueError('Invalid or oversized fixture image')
    return width,height,data

def compare(first,second):
    w,h,a=ppm(first);ww,hh,b=ppm(second)
    if (w,h)!=(ww,hh) or min(w,h)<7:
        raise ValueError('Fixture dimensions differ or are too small for SSIM')
    scores=[];c1=(.01*255)**2;c2=(.03*255)**2;n=49
    for y in range(3,h-3):
        for x in range(3,w-3):
            for channel in range(3):
                aa=[a[(yy*w+xx)*3+channel] for yy in range(y-3,y+4) for xx in range(x-3,x+4)]
                bb=[b[(yy*w+xx)*3+channel] for yy in range(y-3,y+4) for xx in range(x-3,x+4)]
                ma=sum(aa)/n;mb=sum(bb)/n
                va=sum((v-ma)**2 for v in aa)/(n-1);vb=sum((v-mb)**2 for v in bb)/(n-1)
                covariance=sum((v-ma)*(q-mb) for v,q in zip(aa,bb))/(n-1)
                scores.append(((2*ma*mb+c1)*(2*covariance+c2))/((ma*ma+mb*mb+c1)*(va+vb+c2)))
    return {'reference':str(first),'candidate':str(second),'width':w,'height':h,
            'sha256_reference':hashlib.sha256(first.read_bytes()).hexdigest(),
            'sha256_candidate':hashlib.sha256(second.read_bytes()).hexdigest(),
            'differing_channels':sum(v!=q for v,q in zip(a,b)),
            'max_channel_error':max(abs(v-q) for v,q in zip(a,b)),
            'ssim':sum(scores)/len(scores)}

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);args=parser.parse_args()
    pairs=[]
    for candidate in sorted(args.directory.glob('vulkan-*.ppm')):
        suffix=candidate.stem.split('-',2)
        reference=args.directory/('d3d9-'+suffix[2]+'.ppm' if len(suffix)==3 else 'd3d9.ppm')
        pairs.append(compare(reference,candidate))
    passed=bool(pairs) and all(math.isfinite(p['ssim']) and p['ssim']>=.995 for p in pairs)
    report={'schema':1,'scope':__doc__,'stock_rendering_verified':False,'pairs':pairs,'passed':passed}
    (args.directory/'image-parity.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f"Native primitive/material image parity: {len(pairs)} pairs, minimum SSIM {min((p['ssim'] for p in pairs),default=0):.9f}, passed={passed}")
    raise SystemExit(0 if passed else 1)

if __name__=='__main__':main()
