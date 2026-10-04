"""Compare lossless images from exactly mirrored stock resources/draw packets.

Uses 7x7 uniform-window RGB SSIM with sample covariance, no border padding,
8-bit data range, K1=.01, K2=.03. One capture does not qualify other maps/effects.
"""
import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import shutil
from native_screenshot_png import convert_bgra


def compare(width, height, first, second):
    if min(width,height)<7 or max(width,height)>4096 or len(first)!=width*height*4 or len(second)!=len(first):
        raise ValueError('Comparison surfaces are not exact bounded BGRA images')
    stride = width+1
    scores = []
    differing, maximum = 0, 0
    for channel in range(3):
        integrals = [array('d',[0])*((width+1)*(height+1)) for _ in range(5)]
        for y in range(height):
            sums = [0]*5
            for x in range(width):
                at = (y*width+x)*4+channel
                a,b = first[at],second[at]
                differing += a!=b
                maximum = max(maximum,abs(a-b))
                row = (y+1)*stride+x+1
                for n,value in enumerate((a,b,a*a,b*b,a*b)):
                    sums[n] += value
                    integrals[n][row] = integrals[n][row-stride]+sums[n]
        total, count = 0.,0
        for y in range(height-6):
            for x in range(width-6):
                left, right = y*stride+x,y*stride+x+7
                bottom_left, bottom_right = (y+7)*stride+x,(y+7)*stride+x+7
                sa,sb,saa,sbb,sab = (plane[bottom_right]-plane[bottom_left]-plane[right]+plane[left] for plane in integrals)
                ma,mb = sa/49,sb/49
                va,vb,cov = (saa-sa*sa/49)/48,(sbb-sb*sb/49)/48,(sab-sa*sb/49)/48
                c1,c2 = (.01*255)**2,(.03*255)**2
                total += ((2*ma*mb+c1)*(2*cov+c2))/((ma*ma+mb*mb+c1)*(va+vb+c2))
                count += 1
        scores.append(total/count)
    return {'ssim':sum(scores)/3,'channel_ssim_bgr':scores,'differing_rgb_channels':differing,'max_rgb_error':maximum}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    args = parser.parse_args()
    directory = args.directory
    source = directory/'reference.json'
    if not source.is_file():
        source = directory/'fixture-client-home/vulkan/diagnostics/reference.json'
    metadata = json.loads(source.read_text())
    if metadata.get('same_immutable_packets') is not True:
        raise ValueError('Capture does not attest identical resource/draw packets')
    captures = []
    for api in ('d3d9','vulkan'):
        raw = source.parent/f'reference-{api}.bgra'
        pixels = raw.read_bytes()
        captures.append(pixels)
        if source.parent!=directory:
            shutil.copy2(raw,directory/raw.name)
        (directory/f'reference-{api}.png').write_bytes(convert_bgra(metadata['width'],metadata['height'],pixels))
    if source.parent!=directory:
        shutil.copy2(source,directory/'reference.json')
    result = {'scope':__doc__,'width':metadata['width'],'height':metadata['height'],
              'same_immutable_packets':True,'full_stock_coverage_verified':False,
              'sha256_bgra':[hashlib.sha256(p).hexdigest() for p in captures],
              **compare(metadata['width'],metadata['height'],*captures)}
    result['passed'] = math.isfinite(result['ssim']) and result['ssim']>=.995
    (directory/'world-parity.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result),flush=True)
    raise SystemExit(0 if result['passed'] else 1)


if __name__=='__main__':
    main()
