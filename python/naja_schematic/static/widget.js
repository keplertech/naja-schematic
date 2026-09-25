// anywidget front end for naja_schematic.Schematic. widget.py appends this
// to naja-schematic.js (which defines createNajaSchematic) to form the
// widget's ES module. Each rendered view gets its own module instance and
// canvas; protocol messages travel over the widget's comm channel as
// {json: "<message>"} (see JsBridgeProvider.h on the C++ side).

const STYLE_ID = "naja-schematic-style";

function injectStyle() {
  if (document.getElementById(STYLE_ID)) return;
  const style = document.createElement("style");
  style.id = STYLE_ID;
  // !important stops SDL from pinning an inline pixel size on the canvas;
  // main_wasm.cpp follows the CSS size instead.
  style.textContent = `
    .naja-schematic-wrap { position: relative; width: 100%; background: #000; }
    .naja-schematic-canvas { position: absolute; left: 0; top: 0; width: 100% !important;
      height: 100% !important; border: 0; padding: 0; display: block; outline: none; }
    .naja-schematic-status { position: absolute; inset: 0; display: flex; align-items: center;
      justify-content: center; color: #ddd; font: 13px sans-serif; pointer-events: none; }`;
  document.head.appendChild(style);
}

function render({ model, el }) {
  injectStyle();
  const wrap = document.createElement("div");
  wrap.className = "naja-schematic-wrap";
  wrap.style.height = `${model.get("height")}px`;
  const canvas = document.createElement("canvas");
  canvas.className = "naja-schematic-canvas";
  canvas.tabIndex = -1;
  canvas.addEventListener("contextmenu", (e) => e.preventDefault());
  // Keyboard input only goes to the viewer while it has focus.
  canvas.addEventListener("mousedown", () => canvas.focus());
  const status = document.createElement("div");
  status.className = "naja-schematic-status";
  status.textContent = "Loading viewer...";
  wrap.append(canvas, status);
  el.appendChild(wrap);

  let app = null;
  let disposed = false;
  const pending = [];

  const onMessage = (content) => {
    if (!content || typeof content.json !== "string") return;
    if (app) app.deliverMessage(content.json);
    else pending.push(content.json);
  };
  const onHeight = () => { wrap.style.height = `${model.get("height")}px`; };
  model.on("msg:custom", onMessage);
  model.on("change:height", onHeight);

  const shutdown = (m) => {
    try { m._naja_shutdown(); } catch (e) { /* already stopped */ }
    // Give the WebGL context back: browsers cap how many a page may hold.
    canvas.getContext("webgl2")?.getExtension("WEBGL_lose_context")?.loseContext();
  };

  createNajaSchematic({
    canvas,
    najaEmbedded: true,
    najaSend: (json) => model.send({ json }),
  }).then((m) => {
    if (disposed) { shutdown(m); return; }
    app = m;
    status.remove();
    for (const json of pending.splice(0)) app.deliverMessage(json);
  }).catch((e) => {
    status.textContent = `Failed to start the viewer: ${e}`;
  });

  return () => {
    disposed = true;
    model.off("msg:custom", onMessage);
    model.off("change:height", onHeight);
    if (app) shutdown(app);
  };
}

export default { render };
