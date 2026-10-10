/* Eyescire Player — Canvas2D game player for exported HTML games (JavaScript EyescirePlayer) */
(function (global) {
  'use strict';

  const UI_CANVAS_W = 1280;
  const UI_CANVAS_H = 720;

  const VK = {
    LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40,
    A: 65, D: 68, S: 83, W: 87, ESCAPE: 27
  };

  let canvas, ctx;
  let viewW = UI_CANVAS_W, viewH = UI_CANVAS_H;
  let camX = 0, camY = 0, camZoom = 1;
  let uiCx = 0, uiCy = 0, uiScaleX = 1, uiScaleY = 1;
  let entities = [];
  let idToSlot = new Map();
  let selfSlot = -1;
  let keysDown = new Set();
  let mouseX = -1, mouseY = -1, mouseDown = false, mousePressed = false;
  let uiFocus = -1, uiDrag = -1, uiBlink = 0;
  let images = new Map();
  let scriptModules = {};
  let lastTime = 0;
  let dtSeconds = 1 / 60;
  let running = false;
  let projectName = 'Game';

  function rgb(r, g, b) { return `rgb(${r},${g},${b})`; }

  function entIsEmpty(e) {
    if (e.kind && e.kind.indexOf('light_') === 0) return true;   // 3D lights: nothing to draw in 2D
    return e.kind === 'empty' || (e.kind === 'rect' && e.name === 'empty');
  }

  function entIsCamera(e) {
    return e.isCamera || e.kind === 'camera';
  }

  function entIsUi(e) {
    if (!e) return false;
    if (e.kind === 'label' || e.kind === 'ui_label') return true;
    if (e.kind === 'ui_panel' || e.kind === 'ui_button') return true;
    if (e.kind === 'rect' && (e.name === 'panel' || e.name === 'button')) return true;
    return uiName(e, 'panel') || uiName(e, 'button') || uiName(e, 'checkbox')
        || uiName(e, 'toggle') || uiName(e, 'slider') || uiName(e, 'bar') || uiName(e, 'field');
  }

  function uiName(e, want) {
    if (!e) return false;
    return e.kind === want || e.kind === 'ui_' + want || (e.kind === 'rect' && e.name === want);
  }

  function ownsCaption(e) {
    return uiName(e, 'checkbox') || uiName(e, 'toggle') || uiName(e, 'slider') || uiName(e, 'field');
  }

  function gameViewRect() {
    let gw = viewW;
    let gh = viewH;
    if (viewW * UI_CANVAS_H <= viewH * UI_CANVAS_W) {
      gh = Math.floor(viewW * UI_CANVAS_H / UI_CANVAS_W);
    } else {
      gw = Math.floor(viewH * UI_CANVAS_W / UI_CANVAS_H);
    }
    const gx = Math.floor((viewW - gw) / 2);
    const gy = Math.floor((viewH - gh) / 2);
    return { x: gx, y: gy, w: gw, h: gh };
  }

  function snapToMainCamera() {
    let cam = null;
    for (const e of entities) {
      if (e && e.active && entIsCamera(e)) { cam = e; break; }
    }
    const gv = gameViewRect();
    if (!cam) {
      camX = 0; camY = 0; camZoom = 1;
      return;
    }
    let ew = Math.max(4, cam.w);
    let eh = Math.max(4, cam.h);
    const zoomX = gv.w / ew;
    const zoomY = gv.h / eh;
    if (gv.w * eh <= gv.h * ew) {
      camZoom = zoomX;
      camX = cam.x;
      camY = cam.y + eh / 2 - gv.h / (2 * camZoom);
    } else {
      camZoom = zoomY;
      camX = cam.x + ew / 2 - gv.w / (2 * camZoom);
      camY = cam.y;
    }
    if (camZoom < 0.05) camZoom = 0.05;
  }

  function uiLayoutGameView() {
    const gv = gameViewRect();
    uiCx = gv.x;
    uiCy = gv.y;
    uiScaleX = gv.w / UI_CANVAS_W;
    uiScaleY = gv.h / UI_CANVAS_H;
  }

  function worldToScreenX(wx) {
    const gv = gameViewRect();
    return (wx - camX) * camZoom + gv.x;
  }
  function worldToScreenY(wy) {
    const gv = gameViewRect();
    return (wy - camY) * camZoom + gv.y;
  }
  function worldLen(wl) { return wl * camZoom; }
  function uiToScreenX(ux) { return uiCx + ux * uiScaleX; }
  function uiToScreenY(uy) { return uiCy + uy * uiScaleY; }
  function uiLenW(uw) { return uw * uiScaleX; }
  function uiLenH(uh) { return uh * uiScaleY; }

  function loadScene(data) {
    webParts.length = 0;
    entities = [];
    idToSlot = new Map();
    if (!data || !data.entities) return;
    let slot = 0;
    for (const raw of data.entities) {
      const e = {
        active: true,
        slot,
        id: raw.id,
        name: raw.name || '',
        kind: raw.kind || 'rect',
        x: raw.x | 0, y: raw.y | 0,
        w: raw.w | 0, h: raw.h | 0,
        r: raw.r | 0, g: raw.g | 0, b: raw.b | 0,
        sprite: raw.sprite || '',
        text: raw.text || '',
        script: raw.script || '',
        parentId: raw.parentId | 0,
        parentSlot: -1,
        rot: raw.rot | 0,
        isCamera: !!raw.isCamera,
        visible: raw.visible !== 0 && raw.visible !== false,
        btnToggle: raw.btnToggle | 0,
        btnToggled: raw.btnToggled | 0,
        hoverCustom: raw.hoverCustom | 0,
        hoverR: raw.hoverR | 0, hoverG: raw.hoverG | 0, hoverB: raw.hoverB | 0,
        pressCustom: raw.pressCustom | 0,
        pressR: raw.pressR | 0, pressG: raw.pressG | 0, pressB: raw.pressB | 0,
        onCustom: raw.onCustom | 0,
        onR: raw.onR | 0, onG: raw.onG | 0, onB: raw.onB | 0,
        onHoverCustom: raw.onHoverCustom | 0,
        onHoverR: raw.onHoverR | 0, onHoverG: raw.onHoverG | 0, onHoverB: raw.onHoverB | 0,
        uiValue: Math.max(0, Math.min(100, raw.uiValue | 0)),
        z: raw.z | 0,
        sound: raw.sound || '',
        soundLoop: raw.soundLoop ? 1 : 0,
        soundRange: raw.soundRange > 0 ? raw.soundRange : 800,
        a: raw.a === undefined ? 255 : (raw.a | 0),
        emitRate: raw.emitRate === undefined ? 20 : (raw.emitRate | 0),
        emitLife: raw.emitLife === undefined ? 900 : (raw.emitLife | 0),
        emitSpeed: raw.emitSpeed === undefined ? 80 : (raw.emitSpeed | 0),
        emitSpread: raw.emitSpread === undefined ? 40 : (raw.emitSpread | 0),
        emitSize: raw.emitSize === undefined ? 28 : (raw.emitSize | 0),
        emitHD: raw.emitHD ? 1 : 0,
        emitAcc: 0,
        img: null
      };
      entities[slot] = e;
      idToSlot.set(e.id, slot);
      slot++;
    }
    for (const e of entities) {
      if (!e) continue;
      if (e.parentId > 0 && idToSlot.has(e.parentId)) {
        e.parentSlot = idToSlot.get(e.parentId);
      }
    }
  }

  async function preloadImages() {
    const pending = [];
    for (const e of entities) {
      if (!e || !e.sprite) continue;
      const path = e.sprite.replace(/\\/g, '/');
      if (images.has(path)) {
        e.img = images.get(path);
        continue;
      }
      pending.push(new Promise((resolve) => {
        const img = new Image();
        img.onload = () => { images.set(path, img); e.img = img; resolve(); };
        img.onerror = () => { console.warn('[web] missing image:', path); resolve(); };
        img.src = path;
      }));
    }
    await Promise.all(pending);
  }

  const FONT_FACE = 'Consolas, "Courier New", monospace';

  function clampFont(px) {
    if (px < 6) return 6;
    if (px > 96) return 96;
    return px;
  }
  function labelFontPx(sh) { return clampFont(Math.round(sh)); }
  function rectTextFontPx(sh) { return clampFont(Math.round(sh * 0.6)); }

  function entIsButton(e) {
    return e.kind === 'ui_button' || (e.kind === 'rect' && e.name === 'button');
  }

  function roundRectPath(x, y, w, h, r) {
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + w, y, x + w, y + h, r);
    ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r);
    ctx.arcTo(x, y, x + w, y, r);
    ctx.closePath();
  }

  // Mirrors ui_draw_box in engine/ui.nxa: NexaC std/ui "modern" panels and
  // buttons -- rounded, a soft drop shadow, a hairline edge on panels.
  function clampCh(v) { return Math.max(0, Math.min(255, v | 0)); }

  function resolvedTint(e, which) {
    if (which === 2) {
      if (e.onCustom) return [e.onR, e.onG, e.onB];
      return [clampCh(e.r + 36), clampCh(e.g + 36), clampCh(e.b + 36)];
    }
    if (which === 3) {
      const on = resolvedTint(e, 2);
      if (e.onHoverCustom) return [e.onHoverR, e.onHoverG, e.onHoverB];
      return [clampCh(on[0] - 36), clampCh(on[1] - 36), clampCh(on[2] - 36)];
    }
    if (which === 1 && e.pressCustom) return [e.pressR, e.pressG, e.pressB];
    if (which === 0 && e.hoverCustom) return [e.hoverR, e.hoverG, e.hoverB];
    const amt = which === 1 ? 56 : 36;
    return [clampCh(e.r - amt), clampCh(e.g - amt), clampCh(e.b - amt)];
  }

  function buttonFill(e, sx, sy, sw, sh) {
    let c = [e.r, e.g, e.b];
    if (!entIsButton(e)) return c;
    const over = mouseX >= sx && mouseY >= sy && mouseX < sx + sw && mouseY < sy + sh;
    if (e.btnToggle && e.btnToggled) c = resolvedTint(e, 2);
    if (over && mouseDown) {
      if (e.pressCustom) c = [e.pressR, e.pressG, e.pressB];
      else c = [clampCh(c[0] - 28), clampCh(c[1] - 28), clampCh(c[2] - 28)];
    } else if (over) {
      c = resolvedTint(e, e.btnToggle && e.btnToggled ? 3 : 0);
    }
    return c;
  }

  function pollButtons() {
    if (!mousePressed) return;
    let hit = null;
    for (const e of entities) {
      if (!e || !e.active || !entShown(e) || !entIsButton(e) || !e.btnToggle) continue;
      const sx = uiToScreenX(e.x), sy = uiToScreenY(e.y);
      const sw = uiLenW(e.w), sh = uiLenH(e.h);
      if (mouseX >= sx && mouseY >= sy && mouseX < sx + sw && mouseY < sy + sh) hit = e;
    }
    if (hit) hit.btnToggled = hit.btnToggled ? 0 : 1;
  }

  function overUi(e) {
    const sx = uiToScreenX(e.x), sy = uiToScreenY(e.y);
    const sw = uiLenW(e.w), sh = uiLenH(e.h);
    return mouseX >= sx && mouseY >= sy && mouseX < sx + sw && mouseY < sy + sh;
  }

  function setSliderFromMouse(e) {
    const sx = uiToScreenX(e.x), sy = uiToScreenY(e.y);
    const sw = uiLenW(e.w), sh = uiLenH(e.h);
    const L = sliderLayout(e, sx, sy, sw, sh);
    let t = (mouseX - L.tx) / Math.max(1, L.tw);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    e.uiValue = Math.max(0, Math.min(100, Math.round(t * 100)));
  }

  function pollWidgets() {
    if (uiDrag >= 0) {
      const e = entities[uiDrag];
      if (mouseDown && e && e.active) setSliderFromMouse(e);
      else uiDrag = -1;
    }
    if (!mousePressed) return;
    let field = -1, flip = -1, slider = -1;
    for (let i = 0; i < entities.length; i++) {
      const e = entities[i];
      if (!e || !e.active || !entShown(e) || !overUi(e)) continue;
      if (uiName(e, 'field')) field = i;
      if (uiName(e, 'checkbox') || uiName(e, 'toggle')) flip = i;
      if (uiName(e, 'slider')) slider = i;
    }
    uiFocus = field;
    if (flip >= 0) entities[flip].btnToggled = entities[flip].btnToggled ? 0 : 1;
    if (slider >= 0) {
      uiDrag = slider;
      setSliderFromMouse(entities[slider]);
    }
  }

  function roundFill(x, y, w, h, r, style) {
    ctx.fillStyle = style;
    roundRectPath(x, y, w, h, r);
    ctx.fill();
  }

  function widgetFont(sh) {
    ctx.font = rectTextFontPx(sh) + 'px ' + FONT_FACE;
    ctx.textAlign = 'left';
    ctx.textBaseline = 'middle';
  }

  function drawCheck(e, sx, sy, sw, sh) {
    const bs = Math.max(8, Math.min(sw, sh));
    const rad = Math.max(2, Math.trunc(bs / 5));
    if (e.btnToggled) {
      roundFill(sx, sy, bs, bs, rad, rgb(e.r, e.g, e.b));
      ctx.strokeStyle = 'rgb(245,245,245)';
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(sx + bs / 4, sy + bs / 2);
      ctx.lineTo(sx + bs * 0.42, sy + bs * 0.75);
      ctx.lineTo(sx + bs * 0.75, sy + bs / 4);
      ctx.stroke();
    } else {
      roundFill(sx, sy, bs, bs, rad, 'rgb(28,32,40)');
      ctx.strokeStyle = 'rgb(90,96,110)';
      ctx.lineWidth = 1;
      roundRectPath(sx + 0.5, sy + 0.5, bs - 1, bs - 1, rad);
      ctx.stroke();
    }
    widgetFont(sh);
    ctx.fillStyle = 'rgb(232,233,236)';
    ctx.fillText(e.text || e.name || 'Checkbox', sx + bs + 8, sy + sh / 2);
  }

  function drawToggle(e, sx, sy, sw, sh) {
    let pw = sh * 2;
    if (pw > sw) pw = sw;
    const rad = Math.max(2, sh / 2);
    roundFill(sx, sy, pw, sh, rad, e.btnToggled ? rgb(e.r, e.g, e.b) : 'rgb(40,44,54)');
    const ks = Math.max(4, sh - 6);
    const kx = e.btnToggled ? sx + pw - ks - 3 : sx + 3;
    roundFill(kx, sy + (sh - ks) / 2, ks, ks, ks / 2, 'rgb(236,237,240)');
    widgetFont(sh);
    ctx.fillStyle = 'rgb(232,233,236)';
    ctx.fillText(e.text || e.name || 'Toggle', sx + pw + 8, sy + sh / 2);
  }

  function sliderLayout(e, sx, sy, sw, sh) {
    widgetFont(sh);
    const num = String(e.uiValue | 0);
    const numw = ctx.measureText(num).width + 10;
    let labw = 0;
    if (e.text) {
      const lw = ctx.measureText(e.text).width + 8;
      if (lw + numw + 24 <= sw) labw = lw;
    }
    return { num, numw, labw, tx: sx + labw, tw: Math.max(8, sw - labw - numw), ty: sy, th: sh };
  }

  function drawSlider(e, sx, sy, sw, sh) {
    const L = sliderLayout(e, sx, sy, sw, sh);
    widgetFont(sh);
    ctx.fillStyle = 'rgb(210,214,220)';
    if (L.labw > 0) ctx.fillText(e.text, sx, sy + sh / 2);
    const trackH = Math.max(4, Math.min(sh, Math.trunc(sh / 3)));
    const ty = sy + (sh - trackH) / 2;
    roundFill(L.tx, ty, L.tw, trackH, trackH / 2, 'rgb(28,32,40)');
    const v = Math.max(0, Math.min(100, e.uiValue | 0));
    const fw = L.tw * v / 100;
    if (fw > 0) roundFill(L.tx, ty, fw, trackH, trackH / 2, rgb(e.r, e.g, e.b));
    const kr = Math.max(5, Math.min(12, sh / 2));
    const kx = L.tx + L.tw * v / 100 - kr;
    roundFill(kx, sy + sh / 2 - kr, kr * 2, kr * 2, kr, 'rgb(236,237,240)');
    ctx.fillStyle = 'rgb(210,214,220)';
    ctx.fillText(L.num, sx + sw - L.numw + 6, sy + sh / 2);
  }

  function drawBar(e, sx, sy, sw, sh) {
    const rad = Math.max(2, Math.min(8, sh / 2));
    roundFill(sx, sy, sw, sh, rad, 'rgb(28,32,40)');
    const fw = sw * Math.max(0, Math.min(100, e.uiValue | 0)) / 100;
    if (fw > 0) roundFill(sx, sy, fw, sh, rad, rgb(e.r, e.g, e.b));
  }

  function drawField(e, sx, sy, sw, sh) {
    uiBlink++;
    const rad = Math.min(6, sh / 2);
    roundFill(sx, sy, sw, sh, rad, 'rgb(22,26,34)');
    ctx.strokeStyle = (uiFocus >= 0 && entities[uiFocus] === e) ? rgb(e.r, e.g, e.b) : 'rgb(70,76,88)';
    ctx.lineWidth = 1;
    roundRectPath(sx + 0.5, sy + 0.5, sw - 1, sh - 1, rad);
    ctx.stroke();
    widgetFont(sh);
    const focused = uiFocus >= 0 && entities[uiFocus] === e;
    const txt = e.text || '';
    ctx.fillStyle = txt || focused ? 'rgb(232,233,236)' : 'rgb(120,126,138)';
    ctx.fillText(txt || (focused ? '' : 'Text'), sx + 8, sy + sh / 2);
    if (focused && Math.floor(uiBlink / 18) % 2 === 0) {
      const cx = sx + 8 + ctx.measureText(txt).width + 1;
      ctx.fillStyle = 'rgb(232,233,236)';
      ctx.fillRect(cx, sy + 4, 2, sh - 8);
    }
  }

  function drawUiBox(e, sx, sy, sw, sh) {
    if (uiName(e, 'checkbox')) { drawCheck(e, sx, sy, sw, sh); return; }
    if (uiName(e, 'toggle')) { drawToggle(e, sx, sy, sw, sh); return; }
    if (uiName(e, 'slider')) { drawSlider(e, sx, sy, sw, sh); return; }
    if (uiName(e, 'bar')) { drawBar(e, sx, sy, sw, sh); return; }
    if (uiName(e, 'field')) { drawField(e, sx, sy, sw, sh); return; }
    const button = entIsButton(e);
    let rad = Math.trunc((button ? 6 : 10) * uiScaleY);
    if (rad < 2) rad = 2;
    if (rad * 2 > sh) rad = Math.trunc(sh / 2);
    if (rad * 2 > sw) rad = Math.trunc(sw / 2);
    let drop = Math.trunc(2 * uiScaleY);
    if (drop < 1) drop = 1;
    const fill = buttonFill(e, sx, sy, sw, sh);
    ctx.fillStyle = 'rgba(0,0,0,0.275)';
    roundRectPath(sx, sy + drop, sw, sh, rad);
    ctx.fill();
    ctx.fillStyle = rgb(fill[0], fill[1], fill[2]);
    roundRectPath(sx, sy, sw, sh, rad);
    ctx.fill();
    if (!button) {
      const lift = (v) => Math.min(255, v + 22);
      ctx.strokeStyle = rgb(lift(e.r), lift(e.g), lift(e.b));
      ctx.lineWidth = 1;
      roundRectPath(sx + 0.5, sy + 0.5, sw - 1, sh - 1, rad);
      ctx.stroke();
    }
  }

  function drawEntity(e, sx, sy, sw, sh) {
    if (e.kind === 'label') {
      const txt = e.text || e.name;
      ctx.fillStyle = rgb(e.r, e.g, e.b);
      ctx.font = labelFontPx(sh) + 'px ' + FONT_FACE;
      ctx.textAlign = 'left';
      ctx.textBaseline = 'top';
      ctx.fillText(txt, sx, sy);
      return;
    }
    if (e.img) {
      ctx.save();
      if (e.rot) {
        const cx = sx + sw / 2, cy = sy + sh / 2;
        ctx.translate(cx, cy);
        ctx.rotate(e.rot * Math.PI / 180);
        ctx.drawImage(e.img, -sw / 2, -sh / 2, sw, sh);
      } else {
        ctx.drawImage(e.img, sx, sy, sw, sh);
      }
      ctx.restore();
      return;
    }
    ctx.fillStyle = rgb(e.r, e.g, e.b);
    if (e.rot) {
      ctx.save();
      const cx = sx + sw / 2, cy = sy + sh / 2;
      ctx.translate(cx, cy);
      ctx.rotate(e.rot * Math.PI / 180);
      ctx.fillRect(-sw / 2, -sh / 2, sw, sh);
      ctx.restore();
    } else if (entIsUi(e)) {
      drawUiBox(e, sx, sy, sw, sh);
    } else {
      ctx.fillRect(sx, sy, sw, sh);
    }
    if (e.text && sw > 10 && sh > 8 && !e.rot && !ownsCaption(e)) {
      ctx.fillStyle = 'rgb(240,240,240)';
      ctx.font = rectTextFontPx(sh) + 'px ' + FONT_FACE;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText(e.text, sx + sw / 2, sy + sh / 2);
      ctx.textAlign = 'left';
      ctx.textBaseline = 'top';
    }
  }

  function entShown(e) {
    let guard = 0;
    let cur = e;
    while (cur && guard < 64) {
      if (cur.visible === false) return false;
      if (cur.parentSlot < 0) return true;
      cur = entities[cur.parentSlot];
      guard++;
    }
    return true;
  }

  const PART_MAX = 640;
  const webParts = [];
  const burstImgs = new Map();
  let partRng = 1;
  let tintCanvas = null;
  let tintCtx = null;

  function partRnd() {
    partRng = (Math.imul(partRng, 1664525) + 1013904223) >>> 0;
    return (partRng >>> 8) / 16777216;
  }

  function burstImage(path) {
    if (!path) return null;
    let img = burstImgs.get(path);
    if (!img) {
      img = new Image();
      img.src = path;
      burstImgs.set(path, img);
    }
    return img;
  }

  function partSpawn(img, x, y, size, speed, spreadDeg, life, r, g, b, a) {
    if (!img || webParts.length >= PART_MAX) return 0;
    const spread = Math.max(0, spreadDeg) * 0.0174532925;
    const el = partRnd() * spread;
    const az = partRnd() * 6.2831853;
    webParts.push({
      img, x, y,
      vx: Math.sin(el) * Math.cos(az) * speed,
      vy: -Math.cos(el) * speed,
      life, maxLife: life > 0.05 ? life : 0.05,
      size, r, g, b, a
    });
    return 1;
  }

  function partTick() {
    let dt = dtSeconds;
    if (dt < 0.001) dt = 0.016;
    if (dt > 0.05) dt = 0.05;
    for (const e of entities) {
      if (!e || !e.active || e.kind !== 'emitter' || !entShown(e) || !e.sprite) continue;
      if (!e.img) {
        e.img = new Image();
        e.img.src = e.sprite;
      }
      e.emitAcc += dt * (e.emitRate | 0);
      let guard = 8;
      let have = 0;
      for (const p of webParts) if (p.tag === e.slot) have++;
      while (e.emitAcc >= 1 && guard > 0) {
        if (have >= 40) { e.emitAcc = 0; break; }
        e.emitAcc -= 1;
        guard--;
        if (partSpawn(e.img, e.x + e.w * 0.5, e.y + e.h * 0.5, e.emitSize, e.emitSpeed, e.emitSpread, e.emitLife / 1000, e.r, e.g, e.b, e.a)) {
          webParts[webParts.length - 1].tag = e.slot;
          have++;
        }
      }
    }
    for (let i = webParts.length - 1; i >= 0; i--) {
      const p = webParts[i];
      p.life -= dt;
      if (p.life <= 0) { webParts.splice(i, 1); continue; }
      p.vy += 420 * dt;
      p.x += p.vx * dt;
      p.y += p.vy * dt;
    }
  }

  function partBlit(img, sx, sy, sw, sh, r, g, b, a) {
    if (!img || !img.complete || img.naturalWidth < 1 || a < 1 || sw < 1) return;
    ctx.save();
    ctx.globalAlpha = a / 255;
    if (r > 250 && g > 250 && b > 250) {
      ctx.drawImage(img, sx, sy, sw, sh);
      ctx.restore();
      return;
    }
    if (!tintCanvas) {
      tintCanvas = document.createElement('canvas');
      tintCtx = tintCanvas.getContext('2d');
    }
    if (tintCanvas.width < sw || tintCanvas.height < sh) {
      tintCanvas.width = Math.max(tintCanvas.width, sw);
      tintCanvas.height = Math.max(tintCanvas.height, sh);
    }
    tintCtx.clearRect(0, 0, sw, sh);
    tintCtx.globalCompositeOperation = 'source-over';
    tintCtx.drawImage(img, 0, 0, sw, sh);
    tintCtx.globalCompositeOperation = 'multiply';
    tintCtx.fillStyle = 'rgb(' + r + ',' + g + ',' + b + ')';
    tintCtx.fillRect(0, 0, sw, sh);
    tintCtx.globalCompositeOperation = 'destination-in';
    tintCtx.drawImage(img, 0, 0, sw, sh);
    ctx.drawImage(tintCanvas, 0, 0, sw, sh, sx, sy, sw, sh);
    ctx.restore();
  }

  function partDraw() {
    for (const p of webParts) {
      const fade = p.life / p.maxLife;
      const a = p.a * (fade < 0 ? 0 : fade);
      const sw = Math.max(1, p.size * camZoom);
      partBlit(p.img, worldToScreenX(p.x) - sw * 0.5, worldToScreenY(p.y) - sw * 0.5, sw, sw, p.r, p.g, p.b, a);
    }
  }

  function drawWorld() {
    for (const e of entities) {
      if (!e || !e.active || !entShown(e) || entIsEmpty(e) || entIsUi(e) || entIsCamera(e) || e.kind === 'emitter') continue;
      drawEntity(e,
        worldToScreenX(e.x), worldToScreenY(e.y),
        worldLen(e.w), worldLen(e.h));
    }
  }

  function drawUi() {
    uiLayoutGameView();
    for (const e of entities) {
      if (!e || !e.active || !entShown(e) || !entIsUi(e)) continue;
      drawEntity(e,
        uiToScreenX(e.x), uiToScreenY(e.y),
        uiLenW(e.w), uiLenH(e.h));
    }
  }

  function drawScene() {
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.fillStyle = 'rgb(28,32,40)';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    const gv = gameViewRect();
    if (gv.x > 0 || gv.y > 0 || gv.w !== viewW || gv.h !== viewH) {
      ctx.fillStyle = 'rgb(14,16,22)';
      if (gv.y > 0) ctx.fillRect(0, 0, viewW, gv.y);
      if (gv.y + gv.h < viewH) ctx.fillRect(0, gv.y + gv.h, viewW, viewH - (gv.y + gv.h));
      if (gv.x > 0) ctx.fillRect(0, gv.y, gv.x, gv.h);
      if (gv.x + gv.w < viewW) ctx.fillRect(gv.x + gv.w, gv.y, viewW - (gv.x + gv.w), gv.h);
    }
    snapToMainCamera();
    partTick();
    drawWorld();
    partDraw();
    drawUi();
  }

  function aabbOverlap(a, b) {
    return a.x < b.x + b.w && a.x + a.w > b.x &&
           a.y < b.y + b.h && a.y + a.h > b.y;
  }

  function entFirstOverlap(slot, prefix) {
    const me = entities[slot];
    if (!me || !me.active) return -1;
    for (let i = 0; i < entities.length; i++) {
      const o = entities[i];
      if (!o || !o.active || i === slot) continue;
      if (prefix && o.name.substring(0, prefix.length) !== prefix) continue;
      if (aabbOverlap(me, o)) return i;
    }
    return -1;
  }

  /* Mirror native engine_entity_set_x/y: moving a parent shifts its whole
     subtree by the same delta, so a camera parented under the player follows. */
  function propagateDelta(slot, dx, dy) {
    if (dx === 0 && dy === 0) return;
    for (let s = 0; s < entities.length; s++) {
      const e = entities[s];
      if (!e || !e.active || e.parentSlot !== slot) continue;
      e.x += dx;
      e.y += dy;
      propagateDelta(s, dx, dy);
    }
  }

  const api = {
    VK,
    eng_self: () => selfSlot,
    eng_set_self: (s) => { selfSlot = s; },
    eng_dt_seconds: () => dtSeconds,
    eng_log: (msg) => console.log(msg),
    key_down: (vk) => keysDown.has(vk),
    ent_x: (s) => entities[s]?.x ?? 0,
    ent_y: (s) => entities[s]?.y ?? 0,
    ent_w: (s) => entities[s]?.w ?? 0,
    ent_h: (s) => entities[s]?.h ?? 0,
    ent_name: (s) => entities[s]?.name ?? '',
    ent_kind: (s) => entities[s]?.kind ?? '',
    ent_set_x: (s, v) => {
      const e = entities[s];
      if (!e) return;
      const nv = v | 0;
      const dx = nv - e.x;
      if (dx === 0) return;
      e.x = nv;
      propagateDelta(s, dx, 0);
    },
    ent_set_y: (s, v) => {
      const e = entities[s];
      if (!e) return;
      const nv = v | 0;
      const dy = nv - e.y;
      if (dy === 0) return;
      e.y = nv;
      propagateDelta(s, 0, dy);
    },
    ent_destroy: (s) => { if (entities[s]) entities[s].active = false; },
    ent_first_overlap: entFirstOverlap,
    ent_active: (s) => !!(entities[s] && entities[s].active),
    ent_text: (s) => entities[s]?.text ?? '',
    ent_set_text: (s, v) => { if (entities[s]) entities[s].text = String(v ?? ''); },
    ent_ui_value: (s) => entities[s]?.uiValue ?? 0,
    ent_set_ui_value: (s, v) => {
      if (entities[s]) entities[s].uiValue = Math.max(0, Math.min(100, v | 0));
    },
    ent_button_toggled: (s) => entities[s]?.btnToggled ?? 0,
    ent_set_button_toggled: (s, v) => { if (entities[s]) entities[s].btnToggled = v ? 1 : 0; },
    ent_iter_next: (start) => {
      for (let i = Math.max(0, start); i < entities.length; i++) {
        if (entities[i] && entities[i].active) return i;
      }
      return -1;
    },
    emit_burst: (path, x, y, z, r, g, b, hd) => {
      const img = burstImage(path);
      if (!img) return 0;
      let made = 0;
      for (let i = 0; i < 18; i++) {
        made += partSpawn(img, x, y, 28, 120, 50, 0.7, r | 0, g | 0, b | 0, 255);
      }
      return made;
    },
    sfx_play: (path) => {
      const h = playClip(path, { slot: -1, loop: false, spatial: false });
      if (h) liveSounds.push(h);
      return h ? 1 : 0;
    },
    sfx_play_at: (path, x, y) => {
      const h = playClip(path, { slot: -1, loop: false, spatial: true, x: x, y: y, range: 800 });
      if (h) liveSounds.push(h);
      return h ? 1 : 0;
    }
  };

  let audioCtx = null;
  const clipCache = new Map();
  const liveSounds = [];

  function sfxMaster() {
    const n = window.EYESCIRE_SFX;
    if (typeof n === 'number') return Math.max(0, Math.min(100, n)) / 100;
    return 1;
  }

  function ensureAudio() {
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return null;
    if (!audioCtx) audioCtx = new AC();
    if (audioCtx.state === 'suspended') audioCtx.resume();
    return audioCtx;
  }

  function listenerXY() {
    snapToMainCamera();
    const gv = gameViewRect();
    const zoom = camZoom > 0 ? camZoom : 1;
    return { x: camX + gv.w / zoom / 2, y: camY + gv.h / zoom / 2 };
  }

  function loadClip(path) {
    const key = String(path || '').replace(/\\/g, '/');
    if (clipCache.has(key)) return clipCache.get(key);
    const job = fetch(key).then((r) => {
      if (!r.ok) throw new Error('missing');
      return r.arrayBuffer();
    }).then((buf) => {
      const ctx = ensureAudio();
      if (!ctx) throw new Error('no audio');
      return ctx.decodeAudioData(buf);
    });
    clipCache.set(key, job);
    return job;
  }

  function placeSound(handle, x, y, spatial, range) {
    if (!handle.gain || !handle.pan) return;
    let atten = 1;
    let pan = 0;
    if (spatial) {
      const ear = listenerXY();
      const dx = x - ear.x;
      const dy = y - ear.y;
      const dist = Math.hypot(dx, dy);
      const r = range > 0 ? range : 800;
      atten = Math.max(0, 1 - dist / r);
      pan = Math.max(-1, Math.min(1, dx / (r * 0.5)));
    }
    handle.gain.gain.value = sfxMaster() * atten;
    handle.pan.pan.value = pan;
    handle.x = x;
    handle.y = y;
    handle.spatial = spatial ? 1 : 0;
    handle.range = range > 0 ? range : 800;
  }

  function stopSound(handle) {
    handle.dead = true;
    if (handle.src) {
      try { handle.src.stop(); } catch (e) { /* already ended */ }
    }
  }

  function playClip(path, opts) {
    const key = String(path || '').replace(/\\/g, '/');
    if (!key) return null;
    const handle = {
      slot: opts.slot, path: key, loop: opts.loop ? 1 : 0,
      src: null, gain: null, pan: null, dead: false,
      x: opts.x || 0, y: opts.y || 0,
      spatial: opts.spatial ? 1 : 0, range: opts.range > 0 ? opts.range : 800
    };
    loadClip(key).then((buffer) => {
      if (handle.dead || !audioCtx) return;
      const src = audioCtx.createBufferSource();
      src.buffer = buffer;
      src.loop = !!opts.loop;
      const gain = audioCtx.createGain();
      const pan = audioCtx.createStereoPanner();
      src.connect(gain);
      gain.connect(pan);
      pan.connect(audioCtx.destination);
      handle.src = src;
      handle.gain = gain;
      handle.pan = pan;
      placeSound(handle, handle.x, handle.y, handle.spatial, handle.range);
      src.onended = () => { handle.dead = true; };
      src.start();
    }).catch(() => {
      handle.dead = true;
      console.warn('[sfx] could not play', key);
    });
    return handle;
  }

  function startEntitySounds() {
    for (const h of liveSounds) if (h.slot >= 0) stopSound(h);
    for (let i = liveSounds.length - 1; i >= 0; i--) {
      if (liveSounds[i].slot >= 0) liveSounds.splice(i, 1);
    }
    entities.forEach((e, i) => {
      if (!e || !e.active || !e.sound) return;
      const h = playClip(e.sound, {
        slot: i, loop: e.soundLoop,
        x: e.x + e.w / 2, y: e.y + e.h / 2,
        spatial: true, range: e.soundRange
      });
      if (h) liveSounds.push(h);
    });
  }

  function sfxTick() {
    ensureAudio();
    for (let i = liveSounds.length - 1; i >= 0; i--) {
      const h = liveSounds[i];
      if (h.dead) { liveSounds.splice(i, 1); continue; }
      if (h.slot >= 0) {
        const e = entities[h.slot];
        if (!e || !e.active || !e.sound) { stopSound(h); liveSounds.splice(i, 1); continue; }
        placeSound(h, e.x + e.w / 2, e.y + e.h / 2, true, e.soundRange || 800);
      } else if (h.spatial) {
        placeSound(h, h.x, h.y, true, h.range);
      } else {
        placeSound(h, 0, 0, false, 800);
      }
    }
  }

  function dispatchScripts(phase) {
    for (let s = api.ent_iter_next(0); s >= 0; s = api.ent_iter_next(s + 1)) {
      const e = entities[s];
      if (!e.script) continue;
      const mod = scriptModules[e.script];
      if (!mod) continue;
      api.eng_set_self(s);
      if (phase === 'init' && mod.script_init) mod.script_init(api);
      if (phase === 'update' && mod.script_update) mod.script_update(api);
    }
    api.eng_set_self(-1);
  }

  function resizeCanvas() {
    const parent = canvas.parentElement || document.body;
    viewW = Math.max(1, parent.clientWidth | 0);
    viewH = Math.max(1, parent.clientHeight | 0);
    canvas.width = viewW;
    canvas.height = viewH;
  }

  function frame(now) {
    if (!running) return;
    dtSeconds = Math.min(0.05, (now - lastTime) / 1000 || 1 / 60);
    lastTime = now;
    uiLayoutGameView();
    pollWidgets();
    pollButtons();
    dispatchScripts('update');
    sfxTick();
    drawScene();
    mousePressed = false;
    requestAnimationFrame(frame);
  }

  function bindInput() {
    window.addEventListener('keydown', (ev) => {
      keysDown.add(ev.keyCode);
      if (uiFocus >= 0 && entities[uiFocus]) {
        const e = entities[uiFocus];
        if (ev.key === 'Backspace') {
          e.text = (e.text || '').slice(0, -1);
          ev.preventDefault();
        } else if (ev.key === 'Enter' || ev.key === 'Escape') {
          uiFocus = -1;
          ev.preventDefault();
        } else if (ev.key.length === 1 && (e.text || '').length < 80) {
          e.text = (e.text || '') + ev.key;
          ev.preventDefault();
        }
        return;
      }
      if (ev.keyCode === VK.ESCAPE) ev.preventDefault();
    });
    window.addEventListener('keyup', (ev) => keysDown.delete(ev.keyCode));
    window.addEventListener('blur', () => { keysDown.clear(); mouseDown = false; });
    canvas.addEventListener('mousemove', (ev) => {
      const box = canvas.getBoundingClientRect();
      const sx = box.width > 0 ? canvas.width / box.width : 1;
      const sy = box.height > 0 ? canvas.height / box.height : 1;
      mouseX = (ev.clientX - box.left) * sx;
      mouseY = (ev.clientY - box.top) * sy;
    });
    canvas.addEventListener('mousedown', (ev) => {
      if (ev.button !== 0) return;
      mouseDown = true;
      mousePressed = true;
    });
    canvas.addEventListener('mouseup', (ev) => {
      if (ev.button !== 0) return;
      mouseDown = false;
    });
    canvas.addEventListener('mouseleave', () => { mouseX = -1; mouseY = -1; mouseDown = false; });
  }

  async function start(opts) {
    canvas = opts.canvas;
    ctx = canvas.getContext('2d');
    canvas.setAttribute('tabindex', '0');
    canvas.addEventListener('click', () => canvas.focus());
    canvas.addEventListener('pointerdown', () => { ensureAudio(); });
    canvas.focus();
    projectName = opts.projectName || 'Game';
    scriptModules = opts.scripts || {};
    bindInput();
    loadScene(opts.scene);
    resizeCanvas();
    window.addEventListener('resize', () => { resizeCanvas(); });
    await preloadImages();
    dispatchScripts('init');
    startEntitySounds();
    running = true;
    lastTime = performance.now();
    requestAnimationFrame(frame);
  }

  global.EyescirePlayer = { start, VK, UI_CANVAS_W, UI_CANVAS_H };
})(typeof window !== 'undefined' ? window : globalThis);
