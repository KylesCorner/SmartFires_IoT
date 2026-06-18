/**
 * SmartFires map tile layer — browser-side cache/fetch/prefetch.
 *
 * Architecture:
 *  1. Leaflet requests a tile → browser tries Jetson cache (/tiles/z/x/y.png)
 *  2. Cache hit (200): served immediately (fast, works offline)
 *  3. Cache miss (404): browser fetches directly from OSM (browser UA = compliant)
 *  4. After OSM fetch: browser uploads the tile to the Jetson cache (PUT)
 *  5. Pre-fetch: JS walks all tiles in a 2-mile radius and fills the cache
 *
 * The Jetson backend NEVER contacts OSM — all internet access is from the
 * browser, which is what OSM's tile usage policy requires.
 */

// ---------------------------------------------------------------------------
// Tile coordinate math (mirrors tile_cache.py Python implementation)
// ---------------------------------------------------------------------------

const _PREFETCH_ZOOMS = [12, 13, 14, 15, 16, 17];
const _RADIUS_M = 3218.7; // 2 statute miles

function _tileXY(lat, lon, zoom) {
  const latR = (lat * Math.PI) / 180;
  const n = 1 << zoom;
  const x = Math.floor(((lon + 180) / 360) * n);
  const y = Math.floor(
    ((1 - Math.log(Math.tan(latR) + 1 / Math.cos(latR)) / Math.PI) / 2) * n
  );
  return [x, y];
}

function _tilesInRadius(lat, lon, zoom, radiusM = _RADIUS_M) {
  const latR = (lat * Math.PI) / 180;
  const cosLat = Math.cos(latR);
  const dlat = radiusM / 111_320;
  const dlon = radiusM / (111_320 * (cosLat > 1e-6 ? cosLat : 1));

  const n = 1 << zoom;
  const [x0, y0] = _tileXY(lat + dlat, lon - dlon, zoom);
  const [x1, y1] = _tileXY(lat - dlat, lon + dlon, zoom);
  const xMin = Math.max(0, Math.min(x0, x1));
  const xMax = Math.min(n - 1, Math.max(x0, x1));
  const yMin = Math.max(0, Math.min(y0, y1));
  const yMax = Math.min(n - 1, Math.max(y0, y1));

  const tiles = [];
  for (let x = xMin; x <= xMax; x++) {
    for (let y = yMin; y <= yMax; y++) {
      tiles.push([zoom, x, y]);
    }
  }
  return tiles;
}

// ---------------------------------------------------------------------------
// Tile upload helper (browser → Jetson cache)
// ---------------------------------------------------------------------------

async function _uploadTile(z, x, y, imgEl) {
  try {
    const canvas = document.createElement("canvas");
    canvas.width = 256;
    canvas.height = 256;
    canvas.getContext("2d").drawImage(imgEl, 0, 0);
    const blob = await new Promise((res) => canvas.toBlob(res, "image/png"));
    if (!blob) return;
    await fetch(`/tiles/${z}/${x}/${y}.png`, {
      method: "PUT",
      body: blob,
      headers: { "Content-Type": "image/png" },
    });
  } catch (_) {}
}

// ---------------------------------------------------------------------------
// Custom Leaflet tile layer: cache-first, OSM fallback
// ---------------------------------------------------------------------------

const SmartFiresTileLayer = L.TileLayer.extend({
  createTile(coords, done) {
    const tile = document.createElement("img");
    tile.alt = "";
    tile.setAttribute("role", "presentation");

    const { z, x, y } = coords;
    const cacheUrl = `/tiles/${z}/${x}/${y}.png`;
    const osmUrl = `https://tile.openstreetmap.org/${z}/${x}/${y}.png`;

    // On OSM load: notify Leaflet and upload to Jetson cache.
    const onOsmLoad = () => {
      done(null, tile);
      _uploadTile(z, x, y, tile);
    };

    const tryOsm = () => {
      tile.crossOrigin = "anonymous"; // needed for canvas readback (CORS)
      tile.onload = onOsmLoad;
      tile.onerror = () => done(new Error("Tile unavailable"), tile);
      tile.src = osmUrl;
    };

    // Try Jetson cache first (same-origin, no CORS needed).
    tile.onload = () => done(null, tile);
    tile.onerror = tryOsm;
    tile.src = cacheUrl;

    return tile;
  },
});

/**
 * Create the shared SmartFires tile layer.
 * Call this instead of L.tileLayer(...) in page JS.
 */
function createSmartFiresTileLayer() {
  return new SmartFiresTileLayer(null, {
    attribution:
      '© <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors',
    maxZoom: 19,
  });
}

// ---------------------------------------------------------------------------
// Background pre-fetch
// ---------------------------------------------------------------------------

const _prefetchSeen = new Set();
const _prefetchQueue = []; // [[z, x, y], ...]
let _prefetchRunning = false;

/**
 * Queue a background pre-fetch of all tiles within ~2 miles of (lat, lon)
 * for zoom levels 12-17. Deduplicates by location (rounds to ~100 m).
 * Each tile is fetched from OSM by the browser and uploaded to the Jetson
 * cache at ~5 req/s to stay within OSM's acceptable-use policy.
 */
function prefetchTilesForLocation(lat, lon) {
  const key = `${lat.toFixed(3)},${lon.toFixed(3)}`;
  if (_prefetchSeen.has(key)) return;
  _prefetchSeen.add(key);

  for (const zoom of _PREFETCH_ZOOMS) {
    for (const tile of _tilesInRadius(lat, lon, zoom)) {
      _prefetchQueue.push(tile);
    }
  }
  _runPrefetchLoop();
}

async function _runPrefetchLoop() {
  if (_prefetchRunning) return;
  _prefetchRunning = true;

  while (_prefetchQueue.length > 0) {
    const [z, x, y] = _prefetchQueue.shift();

    // Skip if already in Jetson cache.
    try {
      const head = await fetch(`/tiles/${z}/${x}/${y}.png`, { method: "HEAD" });
      if (head.ok) {
        continue;
      }
    } catch (_) {}

    // Fetch from OSM (browser request — compliant) and upload to Jetson.
    try {
      const resp = await fetch(
        `https://tile.openstreetmap.org/${z}/${x}/${y}.png`
      );
      if (resp.ok) {
        const blob = await resp.blob();
        await fetch(`/tiles/${z}/${x}/${y}.png`, {
          method: "PUT",
          body: blob,
          headers: { "Content-Type": "image/png" },
        });
      }
    } catch (_) {}

    // ~5 req/s — well within OSM policy.
    await new Promise((r) => setTimeout(r, 200));
  }

  _prefetchRunning = false;
}
