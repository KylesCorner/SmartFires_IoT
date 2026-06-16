# Offline map tiles

The dashboard's Leaflet maps point at `/tiles/{z}/{x}/{y}.png`, served by FastAPI directly
out of this directory. If this directory is empty (the default), `web/app.py` skips
mounting `/tiles` entirely and Leaflet just shows blank/grey backgrounds — node and base
station markers, and the signal-strength dots on the Map & History page, still render
fine without a basemap.

To get real offline imagery for a deployment site, prepare a tile pyramid **while you
still have internet** (e.g. at the office, before heading to the field) and drop it here
as a `{z}/{x}/{y}.png` folder structure:

- Export from QGIS (`Generate XYZ tiles` processing tool) or MapTiler Desktop for the
  deployment area's bounding box, at the zoom range you expect to use (typically z10–z17
  covers regional-to-node-spacing scales).
- Or use any MBTiles export and unpack it into the same `{z}/{x}/{y}.png` layout with a
  tool like `mb-util`.

No code changes are needed — just populate this directory and restart `smartfires-edge
web`.
