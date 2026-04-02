#!/usr/bin/env python3
"""
SENTRY-RF Field Test Analysis — April 1, 2026
GPS coordinates removed for privacy — only computed distances retained.
"""
import sys, math

f_hz = 915e6

# SITE 1: Walking test (compound, 2-200m, 10 dBm, NLOS)
s1_c8 = [
    (100,1),(110,5),(120,3),(140,5),(150,6),(160,3),(170,2),
    (190,6),(200,4),(220,6),(230,4),(240,5),(260,5),(270,8),
    (280,8),(300,3),(310,3),(320,3),(340,6),(350,5),(370,3),
    (380,4),(391,6),(411,8),(421,9),(431,3),(451,4),(461,4),
    (471,4),(491,5),(501,3),(511,5),(531,6),(541,9),(551,7),
    (571,2),(581,2)
]
s1_c9 = [
    (110,0),(150,0),(170,1),(180,6),(240,11),(250,4),(270,3),
    (320,4),(340,1),(350,3),(391,1),(441,4),(461,1),(511,1),
    (521,1),(541,0),(551,1)
]

# SITE 2: Driving test (rural road, 22 dBm)
# CORRECTED: JJ was near the drive starting position, not the compound.
# Distances: 0-661m (not 2.8-3.3 km as initially calculated)
# (time_s, div, distance_m)
s2_c8 = [
    (20,0,0),(30,0,13),(40,0,18),(50,0,23),(60,0,23),
    (80,0,29),(90,0,36),(100,0,44),(110,1,52),(120,3,60),
    (130,7,69),(150,8,79),(160,5,88),(170,6,99),
    (190,5,121),(200,5,132),(210,4,143),(230,3,143),
    (240,1,155),(250,2,167),(260,3,179),(270,3,192),
    (280,2,204),(290,3,217),(300,3,217),(310,3,217),
    (330,3,244),(340,3,257),(350,2,271),(360,1,285),
    (370,1,299),(390,3,314),(401,3,328),(411,2,343),
    (421,1,417),(431,3,417),(441,0,407),(461,2,400),
    (481,9,381),(491,5,372),(501,3,363),(511,3,353),
    (521,2,344),(531,3,344),(551,3,514),(561,2,526),
    (571,2,526),(581,2,551),(591,1,563),(611,4,587),
    (621,4,600),(631,3,612),(641,3,624),(651,1,624),
    (661,3,637),(671,2,649),(681,0,661),
    # Return trip
    (701,2,22),(711,3,18),(721,3,15),(731,4,15),
    (741,3,15),(751,4,13),(761,4,13),(782,6,14),
    (792,4,14),(802,3,14),(822,14,14),(832,10,15),
    (852,5,15),(862,5,15),
]

print("=" * 65)
print("  SENTRY-RF FIELD TEST ANALYSIS")
print("  April 1, 2026 | Rural North Carolina")
print("=" * 65)

# SITE 1
s1d = [d for _, d in s1_c8]
s1c9a = [d for t, d in s1_c9 if t >= 100]
print(f"\n  SITE 1 (compound, 2-200m, 10 mW, NLOS)")
print(f"  COM8: Pd={100*sum(1 for d in s1d if d>=3)/len(s1d):.0f}% "
      f"avg={sum(s1d)/len(s1d):.1f} max={max(s1d)} n={len(s1d)}")
print(f"  COM9: Pd={100*sum(1 for d in s1c9a if d>=3)/len(s1c9a):.0f}% "
      f"avg={sum(s1c9a)/len(s1c9a):.1f} max={max(s1c9a)} n={len(s1c9a)}")

# SITE 2
out = [(t, d, dist) for t, d, dist in s2_c8 if t <= 681]
ret = [(t, d, dist) for t, d, dist in s2_c8 if t >= 691]
print(f"\n  SITE 2 (rural road, 0-660m, 158 mW)")
print(f"  Outbound: Pd={100*sum(1 for _,d,_ in out if d>=3)/len(out):.0f}% n={len(out)}")
print(f"  Return:   Pd={100*sum(1 for _,d,_ in ret if d>=3)/len(ret):.0f}% n={len(ret)}")
print(f"\n  {'Range':>10s}  {'n':>3s}  {'Pd':>5s}  {'Avg':>5s}  {'Max':>4s}")
for lo in range(0, 700, 100):
    seg = [(d, dist) for _, d, dist in out if lo <= dist < lo+100]
    if seg:
        det = sum(1 for d, _ in seg if d >= 3)
        avg = sum(d for d, _ in seg) / len(seg)
        mx = max(d for d, _ in seg)
        print(f"  {lo:3d}-{lo+99:3d}m  {len(seg):3d}  "
              f"{100*det/len(seg):4.0f}%  {avg:4.1f}  {mx:4d}")

max_det = max(dist for _, d, dist in s2_c8 if d >= 3)
print(f"\n  Max detection: {max_det:.0f}m (div>=3)")

# Link budget
fspl = 20*math.log10(max(max_det,1)) + 20*math.log10(f_hz) - 147.55
rx = 22 - fspl
print(f"  At {max_det:.0f}m: FSPL={fspl:.1f}dB Rx={rx:.1f}dBm margin={rx-(-108):.1f}dB")

print(f"\n" + "=" * 65)
print(f"  CORRECTED SUMMARY")
print(f"  Site 1: 89% Pd at 2-200m, 10 mW, through metal buildings")
print(f"  Site 2: 53% Pd at 0-660m, 158 mW, rural road")
print(f"  Max detection: {max_det:.0f}m at 22 dBm")
print(f"  3D case costs ~50% Pd (COM8=89% vs COM9=41%)")
print(f"=" * 65)
