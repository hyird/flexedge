"""Generate simplified local SVG country paths from Natural Earth 1:50m GeoJSON.

Public-domain source, pinned to ca96624a56bd078437bca8184e78163e5039ad19:
https://github.com/nvkelso/natural-earth-vector/blob/ca96624a56bd078437bca8184e78163e5039ad19/geojson/ne_50m_admin_0_countries.geojson
Usage: python tools/build_world_map.py <downloaded-geojson>
"""
import json
from pathlib import Path
import sys

source = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))

def simplify(points):
    """Douglas-Peucker at 0.35 SVG units; retain closed tiny islands."""
    if len(points) <= 4:
        return points
    start, end = points[0], points[-1]
    dx, dy = end[0]-start[0], end[1]-start[1]
    length = dx*dx+dy*dy
    def distance(point):
        along = max(0, min(1, ((point[0]-start[0])*dx+(point[1]-start[1])*dy)/length)) if length else 0
        return (point[0]-start[0]-along*dx)**2+(point[1]-start[1]-along*dy)**2
    index = max(range(1, len(points)-1), key=lambda item: distance(points[item]))
    if distance(points[index]) <= 0.1225:
        return [start, end]
    return simplify(points[:index+1])[:-1] + simplify(points[index:])

countries = []
countries_by_code = {}
for feature in source['features']:
    properties, geometry = feature['properties'], feature['geometry']
    if properties['ADM0_A3'] == 'ATA':
        continue
    code = properties['ISO_A2_EH']
    if code == '-99':
        code = properties['ADM0_A3']
    polygons = geometry['coordinates'] if geometry['type'] == 'MultiPolygon' else [geometry['coordinates']]
    paths = []
    all_points = []
    for polygon in polygons:
        for ring in polygon:
            points = [((longitude+180)*1000/360, (90-latitude)*500/180) for longitude, latitude in ring]
            all_points.extend(points)
            reduced = simplify(points)
            if len(reduced) < 4:
                reduced = points
            coordinates = [f'{x:.1f},{y:.1f}' for x,y in reduced]
            paths.append('M' + 'L'.join(coordinates) + 'Z')
    bounds = [min(x for x,y in all_points), max(x for x,y in all_points), min(y for x,y in all_points), max(y for x,y in all_points)]
    item = {'code': code, 'name': properties['NAME_ZH'] or properties['NAME'], 'path': ''.join(paths), 'marker': None}
    if max(bounds[1]-bounds[0], bounds[3]-bounds[2]) < 3:
        item['marker'] = [round((bounds[0]+bounds[1])/2, 2), round((bounds[2]+bounds[3])/2, 2)]
    if code in countries_by_code:
        countries_by_code[code]['path'] += item['path']
    else:
        countries.append(item)
        countries_by_code[code] = item
output = Path(__file__).resolve().parents[1] / 'web/features/websites/world_map_paths.json'
output.write_text(json.dumps(countries, ensure_ascii=False, separators=(',', ':'))+'\n', encoding='utf-8')
print(f'Generated {len(countries)} country paths ({output.stat().st_size} bytes)')
