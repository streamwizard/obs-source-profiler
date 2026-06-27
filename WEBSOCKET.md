# WebSocket Broadcast

The Source Profiler plugin can broadcast its per-source/scene/filter performance
stats over **OBS's built-in obs-websocket server** (the one under
*Tools → WebSocket Server Settings*, default port `4455`). It does this by
emitting an obs-websocket **vendor event** — it does **not** run a second server
of its own.

A client (e.g. a web page) connects to obs-websocket like any other client,
subscribes to vendor events, and receives one stats message per send interval.

---

## Enabling it

In OBS: **Tools → Source Profiler**, then in the WebSocket row:

- **Send over WebSocket** — turns broadcasting on/off. Persisted; if left on it
  resumes automatically the next time OBS starts.
- **Content** — choose which categories are sent (Scenes, Groups, Sources,
  Filters, Transitions) and, under **Source types**, restrict to specific input
  kinds (Browser, Display Capture, …).
- **Send every N ms** — how often a message is emitted.

Broadcasting runs in the background and is **independent of the dock window** —
the dock does not need to stay open. The broadcaster always considers *all*
sources, so the Content filter is independent of whatever the dock is viewing.

> Requires obs-websocket (bundled with OBS since v28). If it isn't available the
> plugin logs `obs-websocket not found; stats broadcast disabled` and does
> nothing else.

---

## Connecting (obs-websocket protocol v5)

1. Open a WebSocket to `ws://<host>:4455`.
2. Complete the standard obs-websocket **Identify** handshake (with auth if a
   password is set).
3. Subscribe to **vendor events**. In the `Identify` payload set
   `eventSubscriptions` to include the `Vendors` flag:

   | Flag      | Value      |
   |-----------|------------|
   | `General` | `1 << 0`   |
   | `Vendors` | `1 << 9` (`512`) |

   e.g. `eventSubscriptions = (1 << 0) | (1 << 9)`.

You then receive obs-websocket `VendorEvent` (op `5`) messages. Filter for ours:

- `vendorName` = `"source-profiler"`
- `eventType`  = `"SourceStats"`
- `eventData`  = the stats payload described below

Using the [`obs-websocket-js`](https://github.com/obs-websocket-community-projects/obs-websocket-js)
library:

```js
import OBSWebSocket, { EventSubscription } from 'obs-websocket-js';

const obs = new OBSWebSocket();
await obs.connect('ws://localhost:4455', 'PASSWORD_OR_UNDEFINED', {
  eventSubscriptions: EventSubscription.General | EventSubscription.Vendors,
});

obs.on('VendorEvent', ({ vendorName, eventType, eventData }) => {
  if (vendorName === 'source-profiler' && eventType === 'SourceStats') {
    console.log(eventData.frameTime, eventData.sources);
  }
});
```

---

## Message format (`eventData`)

The payload is a JSON object:

```jsonc
{
  "frameTime": 16.67,   // target frame interval in ms (1000 / output FPS)
  "sources": [ /* array of node objects (see below) */ ]
}
```

### Node object

`sources` is an array of **nodes**. Each node may contain a `children` array of
more nodes (same shape), forming a tree. Nesting follows OBS structure (a scene
contains its sources; a source contains its filters). When the Content filter
excludes a level, its matching descendants are bubbled up to the nearest
included parent, so you may see a flattened list depending on the filter.

| Field             | Type    | Notes |
|-------------------|---------|-------|
| `name`            | string  | The source's user-facing name. |
| `category`        | string  | Stable, locale-independent: `scene`, `group`, `source`, `filter`, or `transition`. **Use this for logic.** |
| `sourceType`      | string  | Localized category label (e.g. "Source", "Scene"). Display only. |
| `displayName`     | string  | The specific kind, localized (e.g. "Browser", "Display Capture"). |
| `kindId`          | string  | Unversioned OBS source id (e.g. `browser_source`). Present for inputs only. |
| `uuid`            | string  | OBS source UUID. Present when the source is resolvable. |
| `width`           | int     | Source width in px. |
| `height`          | int     | Source height in px. |
| `active`          | bool    | Source is active (on program). |
| `rendered`        | bool    | Source is currently being rendered/shown. |
| `enabled`         | bool    | Visible (scene item) / enabled (filter). |
| `async`           | bool    | Async video source (e.g. capture / media). Drives the async fields below. |
| `filter`          | bool    | This node is a filter. |
| `private`         | bool    | Private/internal source. |
| `childCount`      | int     | Number of direct children. |
| `children`        | array   | Child nodes (may be empty). |

#### Timing / load fields

Present when profiler data is available for the node. All durations are in
**milliseconds**; percentages are of the frame budget (`0`–`100+`).

| Field              | Type   | Notes |
|--------------------|--------|-------|
| `tickAvg`          | ms     | Average tick (per-frame logic) time. |
| `tickMax`          | ms     | Max tick time. |
| `renderAvg`        | ms     | Average CPU render time. |
| `renderMax`        | ms     | Max CPU render time. |
| `renderTotal`      | ms     | Sum of CPU render passes per frame. |
| `renderGpuAvg`     | ms     | Average GPU render time. `0` on macOS (no GPU profiling). |
| `renderGpuMax`     | ms     | Max GPU render time. |
| `renderGpuTotal`   | ms     | Sum of GPU render passes per frame. |
| `total`            | ms     | `tickAvg + renderTotal + renderGpuTotal`. |
| `cpuPercentage`    | number | `(renderTotal + tickAvg) / frameInterval * 100`. |
| `gpuPercentage`    | number | `renderGpuTotal / frameInterval * 100`. |
| `totalPercentage`  | number | `total / frameInterval * 100`. |

#### Async fields

Present only when `async` is `true`. FPS fields are frames/second; `*Best` /
`*Worst` are frame intervals in **milliseconds**.

| Field                  | Type   |
|------------------------|--------|
| `asyncInputFps`        | number |
| `asyncRenderedFps`     | number |
| `asyncInputBest`       | ms     |
| `asyncInputWorst`      | ms     |
| `asyncRenderedBest`    | ms     |
| `asyncRenderedWorst`   | ms     |

---

## Example payload

```json
{
  "frameTime": 16.667,
  "sources": [
    {
      "name": "Main Scene",
      "category": "scene",
      "sourceType": "Scene",
      "displayName": "Scene",
      "uuid": "e0b1...c9",
      "width": 1920,
      "height": 1080,
      "active": true,
      "rendered": true,
      "enabled": true,
      "async": false,
      "filter": false,
      "private": false,
      "childCount": 2,
      "tickAvg": 0.012,
      "renderAvg": 0.310,
      "renderTotal": 0.620,
      "renderGpuTotal": 0.880,
      "total": 1.512,
      "cpuPercentage": 3.8,
      "gpuPercentage": 5.3,
      "totalPercentage": 9.1,
      "children": [
        {
          "name": "Webcam",
          "category": "source",
          "sourceType": "Source",
          "displayName": "Video Capture Device",
          "kindId": "dshow_input",
          "uuid": "7a44...01",
          "width": 1280,
          "height": 720,
          "active": true,
          "rendered": true,
          "enabled": true,
          "async": true,
          "filter": false,
          "private": false,
          "childCount": 0,
          "tickAvg": 0.020,
          "renderTotal": 0.210,
          "renderGpuTotal": 0.450,
          "total": 0.680,
          "cpuPercentage": 1.4,
          "gpuPercentage": 2.7,
          "totalPercentage": 4.1,
          "asyncInputFps": 30.0,
          "asyncRenderedFps": 30.0,
          "asyncInputBest": 32.9,
          "asyncInputWorst": 34.1,
          "asyncRenderedBest": 16.6,
          "asyncRenderedWorst": 16.8,
          "children": []
        }
      ]
    }
  ]
}
```

---

## Notes

- **Send rate** can't exceed the dock's data refresh rate — stats are only
  refreshed each refresh interval, so the effective rate is
  `max(refreshInterval, sendInterval)`.
- Fields that don't apply are simply **omitted** (e.g. no async block for
  non-async sources, no timing fields before the profiler has data). Treat
  missing numbers as `0`/unknown.
- A reference consumer lives in [`web/index.html`](web/index.html).
