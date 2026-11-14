"""Haer maker logo ("Hær", set in Climate Crisis, SIL OFL 1.1) for the panel's bottom rail.

Run on a panel SVG to add (or refresh) the logo:  python3 haer_logo.py Panel.svg
make_panel scripts call add_to_file() after writing the panel so it survives regeneration.
The logo is plain filled paths, so Rack needs no font.
"""
import re, sys

# Glyph outlines in font units (y down, baseline at 0). Cap height 750 units.
PATH = "M575 -750H990Q990 -687 990 -624Q990 -561 990 -498Q990 -435 990 -371Q990 -279 990 -186Q990 -93 990 0H575Q575 -61 575 -122Q575 -183 575 -244Q575 -245 574.5 -245.5Q574 -246 573 -246H436Q435 -246 435 -245.5Q435 -245 435 -244Q435 -183 435 -122Q435 -61 435 0H20Q20 -60 20 -120Q20 -180 20 -240Q20 -300 20 -360Q20 -420 20 -480Q20 -540 20 -600Q20 -638 20 -675.5Q20 -713 20 -750H435Q435 -702 435 -653Q435 -604 435 -555.5Q435 -507 435 -458Q435 -457 435 -456.5Q435 -456 436 -456H573Q574 -456 574.5 -456.5Q575 -457 575 -458Q575 -531 575 -604Q575 -677 575 -750ZM1938 30Q1844 30 1762.5 9Q1681 -12 1619.5 -54.5Q1558 -97 1523 -161Q1488 -225 1488 -310Q1484 -330 1469 -346.5Q1454 -363 1431 -375.5Q1408 -388 1377 -394.5Q1346 -401 1309 -401Q1280 -401 1249 -396.5Q1218 -392 1187.5 -383Q1157 -374 1128.5 -361Q1100 -348 1075 -331V-584Q1146 -617 1226 -633.5Q1306 -650 1385 -650Q1464 -650 1536 -633Q1608 -616 1663 -581Q1722 -618 1793 -634Q1864 -650 1938 -650Q2032 -650 2111 -628.5Q2190 -607 2248 -563Q2306 -519 2338 -452.5Q2370 -386 2370 -297H1825Q1825 -297 1824.5 -297Q1824 -297 1824 -297Q1824 -297 1824 -296.5Q1824 -296 1824 -296Q1825 -278 1839.5 -263.5Q1854 -249 1880 -241Q1906 -233 1940 -233Q1989 -233 2020 -250.5Q2051 -268 2052 -297L2361 -196Q2348 -148 2308 -107Q2268 -66 2209.5 -35.5Q2151 -5 2081 12.5Q2011 30 1938 30ZM1493 -23Q1462 0 1411 12.5Q1360 25 1304 25Q1220 25 1154.5 5.5Q1089 -14 1051 -54.5Q1013 -95 1013 -158Q1013 -205 1034 -239Q1055 -273 1092 -295Q1129 -317 1178 -327.5Q1227 -338 1284 -338H1503L1522 -307H1423Q1392 -307 1373 -302.5Q1354 -298 1345 -289Q1336 -280 1336 -266Q1336 -247 1355 -235.5Q1374 -224 1401.5 -223Q1429 -222 1454.5 -234.5Q1480 -247 1493 -275ZM1826 -328H2049Q2050 -328 2050 -328.5Q2050 -329 2050 -330Q2050 -344 2041 -355Q2032 -366 2016 -374Q2000 -382 1980 -386Q1960 -390 1938 -390Q1893 -390 1859 -374Q1825 -358 1825 -330Q1825 -329 1825 -328.5Q1825 -328 1825 -328Q1825 -328 1825.5 -328Q1826 -328 1826 -328ZM2744 -620Q2744 -560 2744 -499.5Q2744 -439 2744 -378.5Q2744 -318 2744 -257Q2744 -193 2744 -129Q2744 -65 2744 0H2398Q2398 -99 2398 -198Q2398 -297 2398 -396Q2398 -452 2398 -508Q2398 -564 2398 -620ZM2743 -238 2797 -507Q2806 -550 2832.5 -578.5Q2859 -607 2895 -622Q2931 -637 2969 -640.5Q3007 -644 3039 -637.5Q3071 -631 3090 -616L3014 -244Q2985 -268 2943.5 -277Q2902 -286 2860 -283.5Q2818 -281 2786 -269Q2754 -257 2743 -238Z"
BOUNDS = (20, -750, 3090, 30)   # xmin, ymin, xmax, ymax
CAP_UNITS = 750.0

def _lum(hexcol):
    h = hexcol.lstrip('#')
    if len(h) == 3: h = ''.join(c*2 for c in h)
    r, g, b = (int(h[i:i+2], 16) / 255 for i in (0, 2, 4))
    return 0.2126*r + 0.7152*g + 0.0722*b

def logo_group(panel_w, bg, x=None, y=126.0, cap=3.0, ink=None, knock=None):
    """SVG <g> for the logo, centred on (x, y) in mm (x defaults to the panel centre).
    bg is the colour behind the logo; it picks the ink (dark on light, cream on dark).
    knock=(y0, y1) paints a bg-coloured patch over that band (mm) so a rail
    decoration such as stitching or a dotted line breaks around the logo."""
    if x is None: x = panel_w / 2
    if ink is None: ink = '#1B1B1B' if _lum(bg) > 0.45 else '#ECE6D8'
    s = cap / CAP_UNITS
    xmin, ymin, xmax, ymax = BOUNDS
    w, h = (xmax - xmin) * s, (ymax - ymin) * s
    left, top = x - w / 2, y - h / 2
    out = ['<g id="haer-logo">']
    if knock:
        out.append(f'<rect x="{left-1.0:.3f}" y="{knock[0]:.3f}" width="{w+2.0:.3f}" height="{knock[1]-knock[0]:.3f}" fill="{bg}"/>')
    out.append(f'<path transform="translate({left - xmin*s:.4f},{top - ymin*s:.4f}) scale({s:.6f})" fill="{ink}" d="{PATH}"/>')
    out.append('</g>')
    return ''.join(out)

def add_to_file(path, **kw):
    """Add (or replace) the logo in a panel SVG. Keyword args as logo_group(); bg defaults
    to the panel's first <rect> fill."""
    svg = open(path, encoding='utf-8').read()
    svg = re.sub(r'<g id="haer-logo">.*?</g>\s*', '', svg, flags=re.S)
    w = float(re.search(r'viewBox="0 0 ([0-9.]+)', svg).group(1))
    bg = kw.pop('bg', None) or re.search(r'<rect[^>]*fill="(#[0-9A-Fa-f]{3,6})"', svg).group(1)
    g = logo_group(w, bg, **kw)
    i = svg.rstrip().rfind('</svg>')
    svg = svg[:i] + g + '\n' + svg[i:]
    open(path, 'w', encoding='utf-8').write(svg)

if __name__ == '__main__':
    for p in sys.argv[1:]:
        add_to_file(p)
