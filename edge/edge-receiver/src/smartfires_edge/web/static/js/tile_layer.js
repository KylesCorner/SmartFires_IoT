/**
 * SmartFires map tile layer — browser-side cache/fetch/prefetch.
 *
 * Tile source: CARTO Voyager (free, non-commercial, OSM data, separate CDN).
 * Using fetch() throughout so HTTP 4xx responses are detected properly and
 * never rendered as tile content (OSM returns 403 as a valid image, which
 * img.onerror cannot detect — fetch() can).
 *
 * Flow per tile:
 *  1. GET /tiles/z/x/y.png → Jetson cache (fast, works offline)
 *  2. If 404 → fetch from CARTO (browser request, not server-side)
 *  3. If CARTO OK → PUT /tiles/z/x/y.png (Jetson stores it for offline)
 *  4. Tile displayed; future loads served from Jetson cache
 *
 * Pre-fetch: when a GPS location is known, proactively fill the cache for
 * the surrounding 2-mile radius at zoom 12-17 at ≤5 req/s.
 */

// ---------------------------------------------------------------------------
// Tile source
// ---------------------------------------------------------------------------

function _tileUrl(z, x, y) {
  // Rotate across CARTO's four subdomains for parallel loading.
  const s = "abcd"[(x + y) % 4];
  return `https://${s}.basemaps.cartocdn.com/rastertiles/voyager/${z}/${x}/${y}.png`;
}

const _TILE_ATTRIBUTION =
  '© <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors ' +
  '© <a href="https://carto.com/attributions">CARTO</a>';

// ---------------------------------------------------------------------------
// Tile coordinate math
// ---------------------------------------------------------------------------

// Proactive prefetch: z13-z15 only (~80 tiles per location, ~40 s at 2 req/s).
// z16-z17 tiles are cached on-demand as the user browses at high zoom.
const _PREFETCH_ZOOMS = [13, 14, 15];
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
// Fetch helpers
// ---------------------------------------------------------------------------

async function _fetchBlob(url) {
  const resp = await fetch(url);
  if (!resp.ok) return null;
  return resp.blob();
}

async function _uploadTile(z, x, y, blob) {
  try {
    await fetch(`/tiles/${z}/${x}/${y}.png`, {
      method: "PUT",
      body: blob,
      headers: { "Content-Type": "image/png" },
    });
  } catch (_) {}
}

// ---------------------------------------------------------------------------
// Custom Leaflet tile layer
// ---------------------------------------------------------------------------

const SmartFiresTileLayer = L.TileLayer.extend({
  /**
   * Override createTile to use fetch() so we get proper HTTP status codes.
   * img.onerror cannot distinguish a 403/404 from a network error — providers
   * sometimes return 4xx with a valid image body (e.g. OSM "Access blocked").
   */
  createTile(coords, done) {
    const tile = document.createElement("img");
    tile.alt = "";
    tile.setAttribute("role", "presentation");

    const { z, x, y } = coords;

    (async () => {
      let blob = null;

      // 1. Try Jetson cache (same-origin; fast; works offline).
      try {
        blob = await _fetchBlob(`/tiles/${z}/${x}/${y}.png`);
      } catch (_) {}

      // 2. Cache miss → fetch from CARTO (browser UA, different CDN from OSM).
      if (!blob) {
        try {
          blob = await _fetchBlob(_tileUrl(z, x, y));
          if (blob) {
            // Store on Jetson for offline use (fire-and-forget).
            _uploadTile(z, x, y, blob.slice(0));
          }
        } catch (_) {}
      }

      if (blob) {
        const url = URL.createObjectURL(blob);
        tile.onload = () => {
          URL.revokeObjectURL(url);
          done(null, tile);
        };
        tile.onerror = () => {
          URL.revokeObjectURL(url);
          done(new Error("Tile render failed"), tile);
        };
        tile.src = url;
      } else {
        // Offline and not cached — show blank tile.
        done(null, tile);
      }
    })();

    return tile;
  },
});

function createSmartFiresTileLayer() {
  return new SmartFiresTileLayer(null, {
    attribution: _TILE_ATTRIBUTION,
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
 * Queue background pre-fetch of all tiles within ~2 miles of (lat, lon)
 * for zoom 12-17. Deduplicates by location (±~100 m). Fetches from CARTO
 * and stores on the Jetson at ≤5 req/s.
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

    // Skip if already cached on Jetson.
    try {
      const head = await fetch(`/tiles/${z}/${x}/${y}.png`, { method: "HEAD" });
      if (head.ok) continue;
    } catch (_) {}

    // Fetch from CARTO and upload to Jetson.
    try {
      const blob = await _fetchBlob(_tileUrl(z, x, y));
      if (blob) {
        await _uploadTile(z, x, y, blob);
      }
    } catch (_) {}

    // ~2 req/s — conservative to avoid rate limiting.
    await new Promise((r) => setTimeout(r, 500));
  }

  _prefetchRunning = false;
}
