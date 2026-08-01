(function(){
  console.log('IIFE starting...');
  
  // Data state manager for decoupling data from rendering
  const simDataState = {
    current: {},
    listeners: [],
    update(newData) {
      this.current = {...this.current, ...newData};
      this.listeners.forEach(fn => fn(this.current));
    },
    subscribe(fn) {
      this.listeners.push(fn);
    },
    unsubscribe(fn) {
      this.listeners = this.listeners.filter(f => f !== fn);
    }
  };
  
  // Expose globally for MQTT client
  window.simDataState = simDataState;

  // Safe access utilities to prevent runtime errors
  const SafeUtils = {
    /**
     * Safely get a rect array, with fallback
     * @param {Object} rects - The rects object (e.g., GJSON.rects)
     * @param {string} name - Region name
     * @param {number[]} fallback - Default if missing [x, y, w, h]
     * @returns {number[]} The rect array
     */
    getRect(rects, name, fallback = [0, 0, 10, 10]) {
      const r = rects?.[name];
      return Array.isArray(r) && r.length >= 4 ? r : fallback;
    },

    /**
     * Safely parse an integer with fallback
     * @param {*} value - Value to parse
     * @param {number} fallback - Default if NaN
     * @returns {number}
     */
    safeParseInt(value, fallback = 0) {
      const parsed = parseInt(value, 10);
      return isNaN(parsed) ? fallback : parsed;
    },

    /**
     * Safely parse a float with fallback
     * @param {*} value - Value to parse
     * @param {number} fallback - Default if NaN
     * @returns {number}
     */
    safeParseFloat(value, fallback = 0) {
      const parsed = parseFloat(value);
      return isNaN(parsed) || !isFinite(parsed) ? fallback : parsed;
    },

    /**
     * Validate canvas context exists
     * @param {HTMLCanvasElement} canvas
     * @returns {CanvasRenderingContext2D|null}
     */
    getContext2D(canvas) {
      if (!canvas) return null;
      const ctx = canvas.getContext('2d');
      if (!ctx) {
        console.warn('Failed to get 2D context');
      }
      return ctx;
    }
  };

  // Expose globally
  window.SafeUtils = SafeUtils;

  let WIDTH = 250, HEIGHT = 122;
  // Rectangles loaded from geometry.json - no hardcoded fallbacks
  // These will be initialized by loadCentralGeometry() before first draw
  let HEADER_NAME = null;
  let HEADER_VERSION = null;
  let HEADER_TIME_CENTER = null;
  let INSIDE_TEMP = null;
  let INSIDE_HUMIDITY = null;
  let INSIDE_PRESSURE = null;
  let OUT_TEMP = null;
  let WEATHER_ICON = null;
  let OUT_PRESSURE = null;
  let OUT_HUMIDITY = null;
  let OUT_WIND = null;

  let canvas = null;
  let ctx = null;
  
  function initCanvas() {
    canvas = document.getElementById('epd');
    if (!canvas) {
      // Canvas not found yet - this is OK, it might be created later
      console.log('Canvas element #epd not found yet');
      return false;
    }
    ctx = canvas.getContext('2d');
    if (!ctx) {
      console.error('Could not get 2D context');
      return false;
    }
    canvas.style.imageRendering = 'pixelated';
    ctx.imageSmoothingEnabled = false;
    
    console.log('Canvas initialized successfully');
    
    // Expose for debugging
    window._canvas = canvas;
    window._ctx = ctx;
    
    return true;
  }
  
  let showWindows = false;
  let stressMode = false;
  let oneBitMode = true;
  let showGrid = false;
  let showRects = false;
  let showLabels = false;
  let simulateGhosting = false;
  let geometryOnly = false; // when true, render only geometry (for labeled mode)
  let labelsOnlyMode = false; // when true, hide normal content and show only region labels
  // removed highlightIssues toggle per feedback
  let GJSON = null;    // centralized geometry JSON
  // Region inspector state
  let regionFilters = { all: true, header: true, temp: true, label: true, footer: true };
  let regionVisible = new Set();
  let regionSelectionActive = false; // false means "all visible" until first explicit selection
  let highlightRect = null; // hovered rect name
  let searchQuery = '';
  let pendingDraw = 0; // rAF id for coalesced redraws
  // Enable spec-only render (always on to keep single source of truth)
  const QS = (typeof window !== 'undefined') ? new URLSearchParams(window.location.search) : new URLSearchParams();
  // Validation state
  let validationEnabled = false;
  let validationIssues = [];
  let renderedContent = {}; // Track what was actually rendered
  let emptyRegions = new Set(); // Track regions with no content
  let missingDataFields = new Set(); // Track data fields that were referenced but missing

  const FONT_STACK = 'Menlo, Consolas, "DM Mono", "Roboto Mono", monospace';
  const SIZE_SMALL = 10; // general small text - reduced from 11 to fit 12px rows
  const SIZE_LABEL = 10; // reduced from 11 to fit better
  const SIZE_TIME = 10; // reduced from 11 to fit 14px header
  const SIZE_BIG = 22;
  const THRESH = 176;

  /**
   * Smart text truncation with ellipsis
   * Handles various truncation strategies for different content types
   */
  function smartTruncate(text, maxWidth, fontSize, weight = 'normal') {
    if (!text || !ctx) return text;
    
    const originalFont = ctx.font;
    ctx.font = `${weight} ${fontSize}px ${FONT_STACK}`;
    
    const textWidth = ctx.measureText(text).width;
    if (textWidth <= maxWidth) {
      ctx.font = originalFont;
      return text;
    }
    
    const ellipsis = '...';
    const ellipsisWidth = ctx.measureText(ellipsis).width;
    const availableWidth = maxWidth - ellipsisWidth;
    
    if (availableWidth <= 0) {
      ctx.font = originalFont;
      return ellipsis;
    }
    
    // Binary search for optimal truncation point
    let left = 0;
    let right = text.length;
    
    while (left < right) {
      const mid = Math.ceil((left + right) / 2);
      const testText = text.substring(0, mid);
      const testWidth = ctx.measureText(testText).width;
      
      if (testWidth <= availableWidth) {
        left = mid;
      } else {
        right = mid - 1;
      }
    }
    
    ctx.font = originalFont;
    
    // Avoid breaking in the middle of a word if possible
    let truncatePoint = left;
    const truncated = text.substring(0, truncatePoint);
    
    // If we broke in the middle of a word, try to back up to the last space
    if (truncatePoint < text.length && text[truncatePoint] !== ' ') {
      const lastSpace = truncated.lastIndexOf(' ');
      if (lastSpace > truncatePoint * 0.5) { // Only use word break if we keep > 50%
        truncatePoint = lastSpace;
      }
    }
    
    return text.substring(0, truncatePoint).trimEnd() + ellipsis;
  }

  /**
   * Smart number formatting for temperatures
   * Truncates decimals before truncating digits
   */
  function formatTemperature(value, maxWidth, fontSize) {
    if (value === undefined || value === null || value === '') return '--';
    
    const num = parseFloat(value);
    if (isNaN(num)) return String(value);
    
    ctx.font = `bold ${fontSize}px ${FONT_STACK}`;
    
    // Try full precision first
    let formatted = num.toFixed(1);
    if (ctx.measureText(formatted).width <= maxWidth) {
      return formatted;
    }
    
    // Try no decimals
    formatted = Math.round(num).toString();
    if (ctx.measureText(formatted).width <= maxWidth) {
      return formatted;
    }
    
    // Just return the integer (might overflow, but that's the minimum)
    return formatted;
  }

  /**
   * Smart value formatting with units
   * Preserves units while truncating value if needed
   */
  function formatValueWithUnit(value, unit, maxWidth, fontSize) {
    if (value === undefined || value === null || value === '') return '';
    
    const num = parseFloat(value);
    if (isNaN(num)) return String(value) + unit;
    
    ctx.font = `${fontSize}px ${FONT_STACK}`;
    const unitWidth = ctx.measureText(unit).width;
    const availableWidth = maxWidth - unitWidth - 2; // 2px gap
    
    if (availableWidth <= 0) return unit; // Just show unit if no space
    
    // Try full precision
    let formatted = num.toFixed(1) + unit;
    if (ctx.measureText(formatted).width <= maxWidth) {
      return formatted;
    }
    
    // Try no decimals
    formatted = Math.round(num).toString() + unit;
    if (ctx.measureText(formatted).width <= maxWidth) {
      return formatted;
    }
    
    return formatted; // Return anyway, may overflow
  }
  // Classify weather strings (HA states, MDI tokens, free-form) to canonical categories
  function classifyWeather(s){
    try{
      const str = String(s||'').toLowerCase().trim();
      if (!str) return 'unknown';
      // MDI tokens
      if (str.startsWith('weather-')){
        const rest = str.slice(8);
        if (rest === 'sunny') return 'sunny';
        if (rest === 'partly-cloudy') return 'partly';
        if (rest === 'cloudy') return 'cloudy';
        if (rest === 'fog') return 'fog';
        if (rest === 'pouring' || rest === 'rainy') return 'rain';
        if (rest === 'snowy') return 'snow';
        if (rest === 'lightning') return 'storm';
        if (rest === 'night') return 'night';
        if (rest === 'night-partly-cloudy') return 'night-partly';
        if (rest === 'windy-variant' || rest === 'windy') return 'wind';
        // heuristics on MDI remainder
        if (rest.includes('night') && rest.includes('cloud')) return 'night-partly';
        if (rest.includes('night')) return 'night';
        if (rest.includes('part')) return 'partly';
        if (rest.includes('cloud')) return 'cloudy';
        if (rest.includes('lightning')||rest.includes('thunder')||rest.includes('storm')) return 'storm';
        if (rest.includes('rain')||rest.includes('pour')) return 'rain';
        if (rest.includes('snow')) return 'snow';
        if (rest.includes('fog')||rest.includes('mist')||rest.includes('haze')) return 'fog';
        if (rest.includes('wind')) return 'wind';
        if (rest.includes('hail')) return 'hail';
      }
      // HA recommended states
      if (str === 'clear-night') return 'night';
      if (str === 'partlycloudy') return 'partly';
      if (str === 'cloudy') return 'cloudy';
      if (str === 'pouring' || str === 'rainy' || str.includes('showers')) return 'rain';
      if (str === 'lightning' || str === 'lightning-rainy') return 'storm';
      if (str === 'snowy' || str.includes('sleet')) return 'snow';
      if (str === 'fog') return 'fog';
      if (str === 'windy' || str === 'windy-variant') return 'wind';
      if (str === 'hail') return 'hail';
      // Generic heuristics
      if (str.includes('night') && str.includes('cloud')) return 'night-partly';
      if (str.includes('night') || str.includes('moon')) return 'night';
      if (str.includes('part')) return 'partly';
      if (str.includes('cloud') || str.includes('overcast')) return 'cloudy';
      if (str.includes('storm') || str.includes('thunder') || str.includes('lightning')) return 'storm';
      if (str.includes('rain') || str.includes('pour') || str.includes('shower') || str.includes('drizzle')) return 'rain';
      if (str.includes('snow')) return 'snow';
      if (str.includes('fog') || str.includes('mist') || str.includes('haze')) return 'fog';
      if (str.includes('wind')) return 'wind';
      if (str.includes('hail')) return 'hail';
      if (str.includes('sun') || str.includes('clear')) return 'sunny';
      return 'sunny';
    }catch(e){ return 'sunny'; }
  }

  // Provide a short, user-facing label for the weather category
  function shortConditionLabel(s){
    const c = classifyWeather(s);
    switch (c){
      case 'night': return 'Night';
      case 'night-partly': return 'Night';
      case 'partly': return 'Partly';
      case 'cloudy': return 'Cloudy';
      case 'rain': return 'Rain';
      case 'snow': return 'Snow';
      case 'storm': return 'Storm';
      case 'fog': return 'Fog';
      case 'wind': return 'Wind';
      case 'hail': return 'Hail';
      default: return 'Sunny';
    }
  }

  // Draw simplified glyph for a given category
  function drawWeatherGlyph(category, startX, startY, iconW, iconH){
    if (!isFinite(iconW) || !isFinite(iconH) || iconW <= 0 || iconH <= 0) return;
    const iconCx = startX + Math.floor(iconW/2);
    const iconCy = startY + Math.floor(iconH/2);
    const minDim = Math.min(iconW, iconH);
    // Ensure stroke styles
    ctx.strokeStyle = '#000';
    ctx.fillStyle = '#000';
    const drawCloud = (yOffset = 0) => {
      // Three-lobe cloud outline with slight base curve for 1-bit clarity
      const r1 = Math.floor(minDim * 0.22);
      const r2 = Math.floor(minDim * 0.28);
      const r3 = Math.floor(minDim * 0.20);
      const cy = startY + Math.floor(iconH * 0.60) + yOffset;
      const cx1 = startX + r1 + 2;
      const cx2 = iconCx;
      const cx3 = startX + iconW - r3 - 2;
      ctx.beginPath();
      ctx.arc(cx1, cy, r1, Math.PI, 0);
      ctx.arc(cx2, cy - Math.floor(r2*0.6), r2, Math.PI*0.9, Math.PI*0.1);
      ctx.arc(cx3, cy, r3, Math.PI, 0);
      // base
      const baseY = cy + Math.floor(r1*0.8);
      ctx.lineTo(cx3 + r3, baseY);
      ctx.lineTo(cx1 - r1, baseY);
      ctx.closePath();
      ctx.stroke();
    };
    const drawSunRays = (r, count = 8) => {
      for (let i=0;i<count;i++){
        const a = (Math.PI*2 * i)/count;
        const dx = Math.round(Math.cos(a) * (r + 4));
        const dy = Math.round(Math.sin(a) * (r + 4));
        ctx.beginPath(); ctx.moveTo(iconCx, iconCy); ctx.lineTo(iconCx + dx, iconCy + dy); ctx.stroke();
      }
    };
    switch (category){
      case 'rain': {
        drawCloud(-2);
        // Raindrops under cloud
        for (let i=0;i<3;i++){
          ctx.beginPath();
          const dropX = startX + 6 + i*6;
          const dropY = iconCy + Math.floor(minDim*0.10);
          ctx.moveTo(dropX, dropY);
          ctx.lineTo(dropX - 3, dropY + 6);
          ctx.stroke();
        }
        break;
      }
      case 'snow': {
        drawCloud(-2);
        for (let i=0;i<3;i++) text(startX + 4 + i*6, iconCy + Math.floor(minDim*0.12), '*', 10);
        break;
      }
      case 'storm': {
        drawCloud(-2);
        ctx.beginPath();
        ctx.moveTo(iconCx-6, iconCy+4);
        ctx.lineTo(iconCx, iconCy-2);
        ctx.lineTo(iconCx-2, iconCy+8);
        ctx.lineTo(iconCx+6, iconCy+4);
        ctx.stroke();
        break;
      }
      case 'fog': {
        drawCloud(0);
        for (let i=0;i<3;i++){
          ctx.beginPath();
          const yy = startY + Math.floor(iconH*0.35) + i*5;
          ctx.moveTo(startX+2, yy);
          ctx.lineTo(startX+iconW-2, yy);
          ctx.stroke();
        }
        break;
      }
      case 'cloudy': {
        drawCloud();
        break;
      }
      case 'partly': {
        drawCloud();
        const r = Math.floor(minDim*0.16);
        const sx = startX + Math.floor(iconW*0.28);
        const sy = startY + Math.floor(iconH*0.32);
        ctx.beginPath(); ctx.arc(sx, sy, r, 0, Math.PI*2); ctx.stroke();
        drawSunRays(r, 6);
        break;
      }
      case 'night': {
        const r = Math.floor(minDim*0.28);
        ctx.beginPath(); ctx.arc(iconCx, iconCy, r, 0, Math.PI*2); ctx.stroke();
        // crescent cutout
        ctx.save();
        ctx.fillStyle = '#fff';
        ctx.beginPath(); ctx.arc(iconCx + Math.floor(r*0.4), iconCy - Math.floor(r*0.2), r, 0, Math.PI*2); ctx.fill();
        ctx.restore();
        break;
      }
      case 'night-partly': {
        drawCloud();
        const r = Math.floor(minDim*0.20);
        const sx = startX + Math.floor(iconW*0.30);
        const sy = startY + Math.floor(iconH*0.35);
        ctx.beginPath(); ctx.arc(sx, sy, r, 0, Math.PI*2); ctx.stroke();
        ctx.save();
        ctx.fillStyle = '#fff';
        ctx.beginPath(); ctx.arc(sx + Math.floor(r*0.5), sy - Math.floor(r*0.2), r, 0, Math.PI*2); ctx.fill();
        ctx.restore();
        break;
      }
      case 'wind': {
        // Gust lines with hooks for direction
        const y0 = iconCy - 3, y1 = iconCy + 4;
        ctx.beginPath(); ctx.moveTo(startX+2, y0); ctx.quadraticCurveTo(iconCx, y0-4, startX+iconW-6, y0); ctx.lineTo(startX+iconW-2, y0+2); ctx.stroke();
        ctx.beginPath(); ctx.moveTo(startX+2, y1); ctx.quadraticCurveTo(iconCx, y1+4, startX+iconW-10, y1); ctx.lineTo(startX+iconW-6, y1+2); ctx.stroke();
        break;
      }
      case 'hail': {
        drawCloud(-2);
        for (let i=0;i<3;i++) text(startX + 6 + i*6, iconCy + Math.floor(minDim*0.12), '•', 10);
        break;
      }
      default: {
        // Sunny: circle + rays
        const r = Math.floor(minDim/3);
        ctx.beginPath(); ctx.arc(iconCx, iconCy, r, 0, Math.PI*2); ctx.stroke();
        drawSunRays(r, 8);
        break;
      }
    }
  }

  // --- High-fidelity SVG icon rendering (MDI) with 1-bit threshold ---
  const MDI_ICON_BY_CATEGORY = {
    sunny: 'weather-sunny',
    partly: 'weather-partly-cloudy',
    cloudy: 'weather-cloudy',
    fog: 'weather-fog',
    rain: 'weather-pouring',
    snow: 'weather-snowy',
    storm: 'weather-lightning',
    night: 'weather-night',
    'night-partly': 'weather-night-partly-cloudy',
    wind: 'weather-windy-variant'
  };
  const __mdiCache = new Map(); // name -> { svgText, bitmaps: Map(sizeKey->canvas) }
  let __redrawPending = false;
  function queueRedrawSoon(){
    if (__redrawPending) return;
    __redrawPending = true;
    const doRedraw = ()=>{
      try{
        __redrawPending = false;
        if (typeof window !== 'undefined' && window.draw && window.lastData){
          window.draw(window.lastData);
        }
      }catch(_){ __redrawPending = false; }
    };
    if (typeof requestAnimationFrame !== 'undefined') requestAnimationFrame(doRedraw);
    else setTimeout(doRedraw, 16);
  }
  // Normalize external SVGs to render with light-mode friendly colors.
  // - Force any use of currentColor to black to avoid OS/browser dark-mode inversions.
  // - Ensure the root <svg> has a black color fallback for any color references.
  function normalizeSvgForLightMode(svgText){
    try{
      let s = String(svgText || '');
      // Replace any currentColor usages so paths/strokes render black
      s = s.replace(/currentColor/gi, '#000');
      // Ensure root svg has a color attribute defaulting to black
      s = s.replace(/<svg([^>]*?)>/i, (m, attrs) => {
        if (/\scolor\s*=/.test(attrs)) return m;
        return `<svg${attrs} color="#000">`;
      });
      return s;
    }catch(_){ return svgText; }
  }
  async function fetchMdiSvgText(name){
    let entry = __mdiCache.get(name);
    if (entry && entry.svgText) return entry.svgText;
    const url = new URL(`../icons/mdi/${name}.svg`, (typeof window!== 'undefined'? window.location.href : '')); 
    try{
      const res = await fetch(url.href);
      const txt = await res.text();
      const norm = normalizeSvgForLightMode(txt);
      entry = entry || { svgText: '', bitmaps: new Map() };
      entry.svgText = norm;
      __mdiCache.set(name, entry);
      return norm;
    }catch(_){ return ''; }
  }
  function thresholdTo1Bit(offCtx, w, h, threshold){
    const img = offCtx.getImageData(0, 0, w, h);
    const data = img.data;
    const t = typeof threshold === 'number' ? threshold : 160; // align with firmware conversion
    for (let i=0;i<data.length;i+=4){
      const r = data[i], g = data[i+1], b = data[i+2], a = data[i+3];
      const y = (0.2126*r + 0.7152*g + 0.0722*b) * (a/255);
      const v = y < t ? 0 : 255;
      data[i] = data[i+1] = data[i+2] = v;
      data[i+3] = 255;
    }
    offCtx.putImageData(img, 0, 0);
  }
  // Standardized icon size for consistent rendering
  const STANDARD_ICON_SIZE = 26; // Fits within 30x32 region with padding

  // Try drawing a baked 1-bit bitmap first to match device output exactly
  function tryDrawBakedBitmap(category, x, y, w, h){
    try{
      const name = MDI_ICON_BY_CATEGORY[category];
      if (!name) return false;
      // The baked icons live in web/icons/, but the page is served from two
      // different roots: scripts/start_simulator.py serves web/ (page at
      // /sim/index.html), while the Playwright tests serve web/sim/ (page at
      // /index.html). Neither single path resolves under both, so try each.
      const base = (typeof window !== 'undefined' ? window.location.href : '');
      const candidates = [
        new URL(`../icons/device_baked/28x28/${name}.png`, base).href,
        new URL(`icons/device_baked/28x28/${name}.png`, base).href,
      ];
      const key = `baked::${name}::${w}x${h}`;
      const entry = __mdiCache.get(key);
      if (entry && entry.bitmaps && entry.bitmaps.get('img')){
        const img = entry.bitmaps.get('img');
        // Native size when it fits (a 28px baked icon in the 30px box draws
        // 1:1, matching the device exactly); only shrink when the box is
        // smaller. Downscaling a 1-bit outline drops rows and breaks 1px
        // strokes, so never scale unless forced.
        const targetSize = Math.min(w - 2, h - 2, Math.max(img.width, img.height));
        const scale = targetSize / Math.max(img.width, img.height);
        const drawW = Math.floor(img.width * scale);
        const drawH = Math.floor(img.height * scale);
        // Center within provided box
        const dx = x + Math.floor((w - drawW)/2);
        const dy = y + Math.floor((h - drawH)/2);
        ctx.imageSmoothingEnabled = false;
        ctx.drawImage(img, dx, dy, drawW, drawH);
        return true;
      }
      // Begin async load, falling through the candidate paths on 404
      (async()=>{
        try{
          let attempt = 0;
          const img = new Image();
          img.onload = ()=>{
            let e = __mdiCache.get(key); if (!e) e = { bitmaps: new Map() };
            e.bitmaps.set('img', img);
            __mdiCache.set(key, e);
            queueRedrawSoon();
          };
          img.onerror = ()=>{
            attempt += 1;
            if (attempt < candidates.length) img.src = candidates[attempt];
          };
          img.src = candidates[attempt];
        }catch(_){ }
      })();
      return false;
    }catch(_){ return false; }
  }

  function tryDrawMdiIcon(category, x, y, w, h){
    try{
      const mdiName = MDI_ICON_BY_CATEGORY[category];
      if (!mdiName) return false;
      // Use standardized icon size for consistent rendering
      const targetW = Math.min(w, STANDARD_ICON_SIZE);
      const targetH = Math.min(h, STANDARD_ICON_SIZE);
      const sizeKey = `${targetW}x${targetH}`;
      const entry = __mdiCache.get(mdiName);
      if (entry){
        const c = entry.bitmaps && entry.bitmaps.get(sizeKey);
        if (c){
          // Center the cached icon within the provided box
          const dx = x + Math.floor((w - targetW)/2);
          const dy = y + Math.floor((h - targetH)/2);
          ctx.drawImage(c, dx, dy);
          return true;
        }
      }
      // Begin async load/rasterize; fallback glyph will draw now
      (async()=>{
        try{
          const svgText = await fetchMdiSvgText(mdiName);
          if (!svgText) return;
          const blob = new Blob([svgText], { type: 'image/svg+xml' });
          const url = URL.createObjectURL(blob);
          const img = new Image();
          img.onload = ()=>{
            try{
              // Oversample at 4x for crisp 1-bit edges, then downscale without smoothing
              const SCALE = 4;
              const hiW = Math.max(1, targetW * SCALE);
              const hiH = Math.max(1, targetH * SCALE);
              const offHi = document.createElement('canvas'); offHi.width = hiW; offHi.height = hiH;
              const ocHi = offHi.getContext('2d'); if (!ocHi) return;
              ocHi.imageSmoothingEnabled = true;
              ocHi.clearRect(0,0,hiW,hiH);
              ocHi.fillStyle = '#fff';
              ocHi.fillRect(0,0,hiW,hiH);
              ocHi.drawImage(img, 0, 0, hiW, hiH);
              thresholdTo1Bit(ocHi, hiW, hiH, 150);
              const off = document.createElement('canvas'); off.width = Math.max(1, targetW); off.height = Math.max(1, targetH);
              const oc = off.getContext('2d'); if (!oc) return;
              oc.imageSmoothingEnabled = false;
              oc.clearRect(0,0,off.width,off.height);
              oc.drawImage(offHi, 0, 0, hiW, hiH, 0, 0, off.width, off.height);
              let e = __mdiCache.get(mdiName); if (!e) { e = { svgText, bitmaps: new Map() }; __mdiCache.set(mdiName, e); }
              e.bitmaps.set(sizeKey, off);
              URL.revokeObjectURL(url);
              queueRedrawSoon();
            }catch(_){ }
          };
          img.onerror = ()=>{ try{ URL.revokeObjectURL(url); }catch(_){ } };
          img.src = url;
        }catch(_){ }
      })();
      return false;
    }catch(_){ return false; }
  }
  
  // UI Validation Functions
  function validateTextOverflow(text, rect, fontSize, weight = 'normal') {
    const canvas = document.createElement('canvas');
    const ctx = canvas.getContext('2d');
    // Include font weight in measurement for accuracy
    ctx.font = `${weight} ${fontSize}px ${FONT_STACK}`;
    const metrics = ctx.measureText(text);
    const textWidth = metrics.width;
    
    // Use more accurate font metrics including ascent/descent
    const actualHeight = (metrics.actualBoundingBoxAscent + metrics.actualBoundingBoxDescent) || (fontSize * 1.2);
    
    // Validate rect before destructuring
    if (!Array.isArray(rect) || rect.length < 4) {
      return [];
    }
    const [_x, _y, w, h] = rect;
    
    const issues = [];
    if (textWidth > w) {
      const overflow = textWidth - w;
      const pct = (overflow / w) * 100;
      // More granular severity levels
      let severity = 'warning';
      if (pct > 50) severity = 'critical';
      else if (pct > 25) severity = 'error';
      else if (pct > 10) severity = 'warning';
      else severity = 'info';
      
      issues.push({
        type: 'text_overflow',
        severity: severity,
        region: rect.name || 'unknown',
        description: `Text "${text}" overflows by ${overflow.toFixed(1)}px (${pct.toFixed(1)}%)`,
        rect: rect,
        suggestion: pct > 25 ? 'Consider truncation or smaller font' : 'Minor overflow, may be acceptable'
      });
    }
    if (actualHeight > h) {
      const overflow = actualHeight - h;
      const pct = (overflow / h) * 100;
      issues.push({
        type: 'text_overflow_vertical',
        severity: pct > 30 ? 'error' : 'warning',
        region: rect.name || 'unknown',
        description: `Text height (${actualHeight.toFixed(1)}px) exceeds bounds by ${overflow.toFixed(1)}px`,
        rect: rect,
        suggestion: 'Reduce font size or increase region height'
      });
    }
    return issues;
  }
  
  function validateCollisions(rects) {
    const issues = [];
    const allowed = new Set([
      'INSIDE_TEMP,INSIDE_LABEL_BOX',
      'INSIDE_LABEL_BOX,INSIDE_TEMP',
      'INSIDE_TEMP,INSIDE_TEMP_INNER',
      'INSIDE_TEMP_INNER,INSIDE_TEMP',
      'INSIDE_TEMP,INSIDE_TEMP_BADGE',
      'INSIDE_TEMP_BADGE,INSIDE_TEMP',
      'OUT_TEMP,OUT_LABEL_BOX',
      'OUT_LABEL_BOX,OUT_TEMP',
      'OUT_TEMP,OUT_TEMP_INNER',
      'OUT_TEMP_INNER,OUT_TEMP',
      'OUT_TEMP,OUT_TEMP_BADGE',
      'OUT_TEMP_BADGE,OUT_TEMP',
      'FOOTER_WEATHER,WEATHER_ICON',
      'WEATHER_ICON,FOOTER_WEATHER',
      'FOOTER_STATUS,INSIDE_PRESSURE',
      'INSIDE_PRESSURE,FOOTER_STATUS'
    ]);
    
    // Region importance weights for severity calculation
    const regionImportance = {
      'INSIDE_TEMP': 10,
      'OUT_TEMP': 10,
      'HEADER_NAME': 8,
      'INSIDE_HUMIDITY': 7,
      'OUT_HUMIDITY': 6,
      'FOOTER_STATUS': 5,
      'FOOTER_WEATHER': 5
    };
    
    const names = Object.keys(rects);
    for (let i = 0; i < names.length; i++) {
      for (let j = i + 1; j < names.length; j++) {
        const name1 = names[i], name2 = names[j];
        const r1 = rects[name1], r2 = rects[name2];
        const [x1, y1, w1, h1] = r1;
        const [x2, y2, w2, h2] = r2;
        
        const overlapX = Math.max(0, Math.min(x1 + w1, x2 + w2) - Math.max(x1, x2));
        const overlapY = Math.max(0, Math.min(y1 + h1, y2 + h2) - Math.max(y1, y2));
        
        if (overlapX > 0 && overlapY > 0) {
          const pair = [name1, name2].sort().join(',');
          if (!allowed.has(pair)) {
            const area = overlapX * overlapY;
            const smaller = Math.min(w1 * h1, w2 * h2);
            const pct = (area / smaller) * 100;
            
            // Calculate importance factor
            const importance1 = regionImportance[name1] || 1;
            const importance2 = regionImportance[name2] || 1;
            const maxImportance = Math.max(importance1, importance2);
            
            // Adjust severity based on overlap percentage and importance
            let severity;
            if (pct > 75) severity = 'critical';
            else if (pct > 50 || (pct > 25 && maxImportance >= 8)) severity = 'error';
            else if (pct > 20 || (pct > 10 && maxImportance >= 6)) severity = 'warning';
            else if (pct > 5) severity = 'info';
            else continue; // Skip very minor overlaps
            
            issues.push({
              type: 'collision',
              severity: severity,
              region: pair,
              description: `${name1} and ${name2} overlap by ${pct.toFixed(1)}% (${overlapX}x${overlapY}px)`,
              rect: [Math.max(x1, x2), Math.max(y1, y2), overlapX, overlapY],
              suggestion: pct > 50 ? 'Major overlap - adjust layout spacing' : 
                         pct > 25 ? 'Significant overlap - consider reducing element sizes' :
                         'Minor overlap - may be intentional for visual effect'
            });
          }
        }
      }
    }
    return issues;
  }
  
  function updateValidationDisplay() {
    const badge = document.getElementById('validationBadge');
    const results = document.getElementById('validationResults');
    const announcement = document.getElementById('validationAnnouncement');
    const summary = document.getElementById('validationSummary');
    
    if (!badge || !results) return;
    
    const critical = validationIssues.filter(i => i.severity === 'critical').length;
    const errors = validationIssues.filter(i => i.severity === 'error').length;
    const warnings = validationIssues.filter(i => i.severity === 'warning').length;
    
    // Update badge
    let announcementText = '';
    if (critical > 0) {
      badge.textContent = `${critical} critical`;
      badge.style.background = '#ff4444';
      badge.style.color = 'white';
      announcementText = `${critical} critical validation issue${critical > 1 ? 's' : ''} detected`;
    } else if (errors > 0) {
      badge.textContent = `${errors} errors`;
      badge.style.background = '#ff8800';
      badge.style.color = 'white';
      announcementText = `${errors} validation error${errors > 1 ? 's' : ''} detected`;
    } else if (warnings > 0) {
      badge.textContent = `${warnings} warnings`;
      badge.style.background = '#ffbb00';
      badge.style.color = 'black';
      announcementText = `${warnings} validation warning${warnings > 1 ? 's' : ''} detected`;
    } else {
      badge.textContent = 'OK';
      badge.style.background = '#44ff44';
      badge.style.color = 'black';
      announcementText = 'No validation issues';
    }
    
    // Announce to screen readers
    if (announcement) {
      announcement.textContent = announcementText;
    }
    if (summary) {
      const totalIssues = critical + errors + warnings;
      summary.textContent = `${totalIssues} validation issue${totalIssues !== 1 ? 's' : ''}: ${critical} critical, ${errors} errors, ${warnings} warnings`;
    }
    
    // Update results list with enhanced formatting and suggestions
    if (validationIssues.length === 0) {
      results.innerHTML = '<div style="color:#666;">No validation issues detected</div>';
    } else {
      const grouped = {};
      validationIssues.forEach(issue => {
        if (!grouped[issue.severity]) grouped[issue.severity] = [];
        grouped[issue.severity].push(issue);
      });
      
      let html = '';
      const severityOrder = ['critical', 'error', 'warning', 'info'];
      const severityColors = {
        critical: '#ff4444',
        error: '#ff8800',
        warning: '#ffbb00',
        info: '#4488ff'
      };
      
      severityOrder.forEach(severity => {
        if (!grouped[severity]) return;
        
        html += `<div style="margin-top:8px;border-left:3px solid ${severityColors[severity]};padding-left:8px;">`;
        grouped[severity].forEach(issue => {
          const icon = {
            critical: '🔴',
            error: '🟠',
            warning: '🟡',
            info: 'ℹ️'
          }[issue.severity];
          
          // Add data-region for hover highlighting; use first region token if multiple
          const regionAttr = String(issue.region || '').replace(/"/g, '&quot;');
          html += `<div class="issue-item" data-region="${regionAttr}" style="margin-bottom:6px;">
            <div><span style="font-weight:bold;">${icon} ${issue.type.replace(/_/g, ' ').toUpperCase()}</span> 
            <span style="color:#666;font-size:10px;">[${issue.region}]</span></div>
            <div style="margin-left:20px;font-size:11px;">${issue.description}</div>`;
          
          if (issue.suggestion) {
            html += `<div style="margin-left:20px;font-size:10px;color:#666;font-style:italic;">
              💡 ${issue.suggestion}</div>`;
          }
          
          // Add action buttons for certain issues
          if (issue.type === 'text_overflow' && issue.severity !== 'info') {
            html += `<div style="margin-left:20px;margin-top:2px;">
              <button onclick="window.applyValidationFix('truncate', '${issue.region}')" 
                      style="font-size:10px;padding:1px 4px;color:var(--color-text);background:var(--color-surface);">Apply Truncation</button>
              <button onclick="window.applyValidationFix('smaller_font', '${issue.region}')" 
                      style="font-size:10px;padding:1px 4px;color:var(--color-text);background:var(--color-surface);">Reduce Font</button>
            </div>`;
          }
          
          html += '</div>';
        });
        html += '</div>';
      });
      
      results.innerHTML = html;
      // Wire hover highlighting for issues → highlightRect and toggle showRects for visibility
      try {
        const items = results.querySelectorAll('.issue-item');
        items.forEach(el => {
          const regionStr = el.getAttribute('data-region') || '';
          // Parse first region from patterns like "A,B" or "A vs B"
          let first = regionStr.split(',')[0];
          if (first.includes(' vs ')) first = first.split(' vs ')[0];
          first = first.trim();
          el.addEventListener('mouseenter', () => {
            if (!first || !GJSON || !GJSON.rects || !GJSON.rects[first]) return;
            window.__prevShowRects = showRects;
            showRects = true;
            const rEl = document.getElementById('showRects');
            if (rEl) rEl.checked = true;
            highlightRect = first;
            scheduleDraw();
          });
          el.addEventListener('mouseleave', () => {
            highlightRect = null;
            if (window.__prevShowRects === false) {
              showRects = false;
              const rEl = document.getElementById('showRects');
              if (rEl) rEl.checked = false;
            }
            scheduleDraw();
          });
        });
      } catch(e) { /* non-fatal */ }
    }
  }
  
  // Validate data ranges and formats
  function validateDataRanges(data) {
    const issues = [];
    
    // Temperature range checks
    if (data.inside_temp_f !== undefined) {
      const temp = parseFloat(data.inside_temp_f);
      if (isNaN(temp)) {
        issues.push({
          type: 'invalid_data_format',
          severity: 'error',
          region: 'INSIDE_TEMP',
          description: `Invalid temperature format: "${data.inside_temp_f}"`,
          suggestion: 'Use numeric value'
        });
      } else if (temp < -40 || temp > 150) {
        issues.push({
          type: 'data_out_of_range',
          severity: 'warning',
          region: 'INSIDE_TEMP',
          description: `Temperature ${temp}°F is outside realistic range (-40 to 150°F)`,
          suggestion: 'Check sensor calibration'
        });
      }
    }
    
    // Humidity range checks
    if (data.inside_hum_pct !== undefined) {
      const hum = parseFloat(data.inside_hum_pct);
      if (!isNaN(hum) && (hum < 0 || hum > 100)) {
        issues.push({
          type: 'data_out_of_range',
          severity: 'error',
          region: 'INSIDE_HUMIDITY',
          description: `Humidity ${hum}% is outside valid range (0-100%)`,
          suggestion: 'Humidity must be between 0 and 100%'
        });
      }
    }
    
    // Battery percentage checks
    if (data.battery_percent !== undefined) {
      const batt = parseFloat(data.battery_percent);
      if (!isNaN(batt) && (batt < 0 || batt > 100)) {
        issues.push({
          type: 'data_out_of_range',
          severity: 'warning',
          region: 'FOOTER_STATUS',
          description: `Battery ${batt}% is outside valid range`,
          suggestion: 'Battery percentage must be 0-100%'
        });
      }
    }
    
    // Pressure range checks
    if (data.pressure_hpa !== undefined) {
      const pressure = parseFloat(data.pressure_hpa);
      if (!isNaN(pressure) && (pressure < 850 || pressure > 1100)) {
        issues.push({
          type: 'data_out_of_range',
          severity: 'warning',
          region: 'INSIDE_PRESSURE',
          description: `Pressure ${pressure} hPa is unusual (typical: 950-1050 hPa)`,
          suggestion: 'Verify barometer reading'
        });
      }
    }
    
    // CO2 range checks
    if (data.co2_ppm !== undefined) {
      const co2 = parseFloat(data.co2_ppm);
      if (!isNaN(co2)) {
        if (co2 < 400) {
          issues.push({
            type: 'data_out_of_range',
            severity: 'info',
            region: 'data',
            description: `CO2 ${co2} ppm is below outdoor levels (400 ppm)`,
            suggestion: 'Sensor may need calibration'
          });
        } else if (co2 > 5000) {
          issues.push({
            type: 'data_out_of_range',
            severity: 'warning',
            region: 'data',
            description: `CO2 ${co2} ppm is dangerously high`,
            suggestion: 'Check ventilation or sensor accuracy'
          });
        }
      }
    }
    
    validationIssues.push(...issues);
    return issues;
  }
  
  // Validate baseline alignment between related regions
  function validateBaselineAlignment(renderedContent) {
    const issues = [];
    const alignmentGroups = [
      ['INSIDE_TEMP', 'OUT_TEMP'],
      ['INSIDE_HUMIDITY', 'OUT_HUMIDITY'],
      ['HEADER_NAME', 'HEADER_VERSION'],
      ['FOOTER_STATUS', 'FOOTER_WEATHER']
    ];
    
    for (const group of alignmentGroups) {
      const baselines = [];
      for (const regionName of group) {
        if (renderedContent[regionName] && renderedContent[regionName].actualBounds) {
          const bounds = renderedContent[regionName].actualBounds;
          baselines.push({ region: regionName, y: bounds.y });
        }
      }
      
      if (baselines.length > 1) {
        const maxDiff = Math.max(...baselines.map(b => b.y)) - Math.min(...baselines.map(b => b.y));
        if (maxDiff > 2) { // Allow 2px tolerance
          issues.push({
            type: 'baseline_misalignment',
            severity: maxDiff > 5 ? 'error' : 'warning',
            region: group.join(' vs '),
            description: `Baseline misalignment of ${maxDiff.toFixed(1)}px between ${group.join(' and ')}`,
            suggestion: 'Adjust vertical positioning for consistent alignment',
            rect: null
          });
        }
      }
    }
    
    return issues;
  }
  
  function validateCenterlineProximity(rects, spec, minGap = 4) {
    const issues = [];
    
    // Find center divider X position from chrome spec
    let centerX = 125; // default
    if (spec && spec.components && spec.components.chrome) {
      for (const op of spec.components.chrome) {
        if (op.op === 'line' && op.from && op.to) {
          const [x1, y1] = op.from;
          const [x2, y2] = op.to;
          // Vertical line check
          if (x1 === x2 && y1 !== y2 && x1 > 100 && x1 < 150) {
            centerX = x1;
            break;
          }
        }
      }
    }
    
    // Check INSIDE_TEMP proximity to center
    if (rects.INSIDE_TEMP) {
      const [x, y, w, h] = rects.INSIDE_TEMP;
      const rightEdge = x + w;
      const gap = centerX - rightEdge;
      
      if (gap < 0) {
        issues.push({
          type: 'centerline_collision',
          severity: 'critical',
          region: 'INSIDE_TEMP',
          description: `INSIDE_TEMP crosses center divider (gap: ${gap}px)`,
          rect: [centerX - 3, y, 6, h]  // Fixed 6px width centered on divider
        });
      } else if (gap < minGap) {
        issues.push({
          type: 'centerline_proximity',
          severity: 'warning',
          region: 'INSIDE_TEMP',
          description: `INSIDE_TEMP too close to center (gap: ${gap}px, min: ${minGap}px)`,
          rect: [centerX - 3, y, 6, h]  // Fixed 6px width for visibility
        });
      }
    }
    
    // Check OUT_TEMP proximity to center
    if (rects.OUT_TEMP) {
      const [x, y, _w, h] = rects.OUT_TEMP;
      const leftEdge = x;
      const gap = leftEdge - centerX;
      
      if (gap < 0) {
        issues.push({
          type: 'centerline_collision',
          severity: 'critical',
          region: 'OUT_TEMP',
          description: `OUT_TEMP crosses center divider (gap: ${gap}px)`,
          rect: [centerX - 3, y, 6, h]  // Fixed 6px width centered on divider
        });
      } else if (gap < minGap) {
        issues.push({
          type: 'centerline_proximity',
          severity: 'warning',
          region: 'OUT_TEMP',
          description: `OUT_TEMP too close to center (gap: ${gap}px, min: ${minGap}px)`,
          rect: [centerX - 3, y, 6, h]  // Fixed 6px width for visibility
        });
      }
    }
    
    return issues;
  }
  
  function validateLabelTempProximity(rects, minWarnOverlapPx = 1, minCritOverlapPx = 4) {
    const issues = [];
    const LABEL_HEIGHT = 12; // Standard label height
    
    // Check INSIDE label to INSIDE_TEMP proximity
    if (rects.INSIDE_TEMP && rects.INSIDE_HUMIDITY) {
      const [tx, ty, tw, _th] = rects.INSIDE_TEMP;
      const [_hx, hy, _hw, _hh] = rects.INSIDE_HUMIDITY;
      
      // Label is at top of humidity region
      const labelBottom = hy + LABEL_HEIGHT;
      const tempTop = ty;
      const overlap = labelBottom - tempTop; // Positive = overlapping
      
      if (overlap >= minCritOverlapPx) {
        issues.push({
          type: 'label_temp_collision',
          severity: 'critical',
          region: 'INSIDE_TEMP',
          description: `INSIDE label overlaps temperature by ${overlap}px`,
          rect: [tx, tempTop, tw, overlap]
        });
      } else if (overlap >= minWarnOverlapPx) {
        issues.push({
          type: 'label_temp_overlap',
          severity: 'warning',
          region: 'INSIDE_TEMP',
          description: `INSIDE label overlaps temperature by ${overlap}px`,
          rect: [tx, tempTop, tw, Math.max(1, overlap)]
        });
      }
    }
    
    // Check OUTSIDE label to OUT_TEMP proximity
    if (rects.OUT_TEMP && rects.OUT_HUMIDITY) {
      const [tx, ty, tw, _th] = rects.OUT_TEMP;
      const [_hx, hy, _hw, _hh] = rects.OUT_HUMIDITY;
      
      const labelBottom = hy + LABEL_HEIGHT;
      const tempTop = ty;
      const overlap = labelBottom - tempTop; // Positive = overlapping
      
      if (overlap >= minCritOverlapPx) {
        issues.push({
          type: 'label_temp_collision',
          severity: 'critical',
          region: 'OUT_TEMP',
          description: `OUTSIDE label overlaps temperature by ${overlap}px`,
          rect: [tx, tempTop, tw, overlap]
        });
      } else if (overlap >= minWarnOverlapPx) {
        issues.push({
          type: 'label_temp_overlap',
          severity: 'warning',
          region: 'OUT_TEMP',
          description: `OUTSIDE label overlaps temperature by ${overlap}px`,
          rect: [tx, tempTop, tw, Math.max(1, overlap)]
        });
      }
    }
    
    return issues;
  }
  
  function validateGridAlignment(rects, gridSize = 4) {
    const issues = [];
    
    for (const [name, rect] of Object.entries(rects)) {
      const [x, y, w, h] = rect;
      const misaligned = [];
      
      if (x % gridSize !== 0) misaligned.push(`x=${x}`);
      if (y % gridSize !== 0) misaligned.push(`y=${y}`);
      if (w % gridSize !== 0) misaligned.push(`w=${w}`);
      if (h % gridSize !== 0) misaligned.push(`h=${h}`);
      
      if (misaligned.length > 0) {
        issues.push({
          type: 'grid_misalignment',
          severity: 'info',
          region: name,
          description: `Not aligned to ${gridSize}px grid: ${misaligned.join(', ')}`,
          rect: rect,
          suggestion: `Align to ${gridSize}px grid for consistency`
        });
      }
    }
    
    return issues;
  }
  
  function validateWeatherIconAlignment(rects, maxLeftPadding = 8) {
    const issues = [];
    
    if (rects.WEATHER_ICON && rects.FOOTER_WEATHER) {
      const [iconX, iconY, _iconW, iconH] = rects.WEATHER_ICON;
      const [footerX, footerY, _footerW, footerH] = rects.FOOTER_WEATHER;
      
      // Check if icon is left-justified within footer
      const leftPadding = iconX - footerX;
      
      if (leftPadding > maxLeftPadding) {
        issues.push({
          type: 'weather_icon_alignment',
          severity: 'warning',
          region: 'WEATHER_ICON',
          description: `Weather icon not left-justified (padding: ${leftPadding}px, max: ${maxLeftPadding}px)`,
          rect: rects.WEATHER_ICON,
          suggestion: 'Move icon to left edge of footer region'
        });
      }
      
      // Check vertical centering
      const footerCenterY = footerY + footerH / 2;
      const iconCenterY = iconY + iconH / 2;
      const verticalOffset = Math.abs(footerCenterY - iconCenterY);
      
      if (verticalOffset > 4) {
        issues.push({
          type: 'weather_icon_vertical',
          severity: 'info',
          region: 'WEATHER_ICON',
          description: `Weather icon not vertically centered (offset: ${verticalOffset}px)`,
          rect: rects.WEATHER_ICON
        });
      }
    }
    
    return issues;
  }
  
  function runValidation() {
    if (!validationEnabled || !GJSON || !GJSON.rects) return;
    
    validationIssues = [];
    
    // Check for collisions (filter out internal helper rects)
    const rectsToValidate = {};
    Object.entries(GJSON.rects).forEach(([name, rect]) => {
      if (!name.includes('_INNER') && !name.includes('_BADGE') && !name.includes('LABEL_BOX')) {
        rectsToValidate[name] = rect;
      }
    });
    validationIssues.push(...validateCollisions(rectsToValidate));
    
    // Check rendered content for overflow and incomplete data
    for (const [regionName, content] of Object.entries(renderedContent)) {
      if (GJSON.rects[regionName] && content.text) {
        // Skip validation for internal helper regions but validate the actual temp inner regions
        // Skip badge and label box regions as they're internal helpers
        if (regionName.includes('_BADGE') || regionName.includes('LABEL_BOX')) {
          continue;
        }
        // Skip outside metric regions that have known tight bounds (but not temp regions)
        if (regionName === 'OUT_PRESSURE' ||
            regionName === 'OUT_HUMIDITY' || regionName === 'OUT_WIND') {
          continue;
        }
        const rect = GJSON.rects[regionName];
        rect.name = regionName;
        const issues = validateTextOverflow(content.text, rect, content.fontSize || SIZE_SMALL, content.weight || 'normal');
        validationIssues.push(...issues);
        
        // Check for incomplete data (e.g., "mph" without a number, "%" without a value)
        const text = content.text.trim();
        if (text === 'mph' || text === 'km/h' || text === '%' || 
            text === '% RH' || text === 'hPa' || text === 'ppm' ||
            text.match(/^\s*(mph|km\/h|%|hPa|ppm|mm)\s*$/)) {
          validationIssues.push({
            type: 'incomplete_data',
            severity: 'warning',
            region: regionName,
            description: `Region shows units without value: "${text}"`,
            rect: rect,
            suggestion: 'Check data source or add fallback value'
          });
        }
        
        // Check for truncated numbers
        if (text.includes('...') && /\d/.test(text)) {
          validationIssues.push({
            type: 'truncated_number',
            severity: 'error',
            region: regionName,
            description: `Number appears truncated: "${text}"`,
            rect: rect,
            suggestion: 'Increase region width or reduce decimal places'
          });
        }
      }
    }
    
    // Check baseline alignment
    validationIssues.push(...validateBaselineAlignment(renderedContent));
    
    // Check centerline proximity
    validationIssues.push(...validateCenterlineProximity(rectsToValidate, window.UI_SPEC));
    
    // Check label-temperature proximity
    validationIssues.push(...validateLabelTempProximity(rectsToValidate));
    
    // Check grid alignment (info level)
    validationIssues.push(...validateGridAlignment(rectsToValidate));
    
    // Check weather icon alignment
    validationIssues.push(...validateWeatherIconAlignment(rectsToValidate));
    
    // Check for empty regions that should have content
    // Smart expected content based on available data
    const expectedContent = new Set(['HEADER_NAME', 'HEADER_VERSION', 'INSIDE_TEMP', 'INSIDE_HUMIDITY']);
    
    // Add conditional expectations based on actual data
    if (lastData.time || lastData.time_hhmm) {
      expectedContent.add('HEADER_TIME_CENTER');
    }
    if (lastData.pressure_hpa !== undefined && lastData.pressure_hpa !== null) {
      expectedContent.add('INSIDE_PRESSURE');
    }
    if (lastData.outside_temp_f !== undefined) {
      expectedContent.add('OUT_TEMP');
    }
    if (lastData.outside_hum_pct !== undefined) {
      expectedContent.add('OUT_HUMIDITY');
    }
    if (lastData.wind_mps !== undefined || lastData.wind_mph !== undefined) {
      expectedContent.add('OUT_WIND');
    }
    if (lastData.weather) {
      expectedContent.add('WEATHER_ICON');
      expectedContent.add('FOOTER_WEATHER');
    }
    if (lastData.battery_percent !== undefined || lastData.ip || lastData.days !== undefined) {
      expectedContent.add('FOOTER_STATUS');
    }
    
    // Coverage thresholds for different region types
    const COVERAGE_THRESHOLDS = {
      WEATHER_ICON: 5,      // Icon regions
      INSIDE_TEMP: 10,      // Large text regions
      OUT_TEMP: 10,
      HEADER_NAME: 10,      // Header text
      HEADER_VERSION: 10,
      HEADER_TIME_CENTER: 10,
      INSIDE_HUMIDITY: 8,   // Smaller text regions
      INSIDE_PRESSURE: 8,
      OUT_HUMIDITY: 8,
      OUT_WIND: 8,
      OUT_PRESSURE: 8,
      FOOTER_STATUS: 8,     // Footer regions
      FOOTER_WEATHER: 8,
      default: 10           // Default threshold
    };
    
    for (const regionName of expectedContent) {
      if (!renderedContent[regionName] && GJSON.rects[regionName]) {
        validationIssues.push({
          type: 'empty_region',
          severity: 'warning',
          region: regionName,
          description: `Region has no rendered content`,
          rect: GJSON.rects[regionName]
        });
      } else if (renderedContent[regionName] && GJSON.rects[regionName]) {
        // Check if content is effectively empty (just whitespace or placeholder)
        const text = (renderedContent[regionName].text || '').trim();
        if (text === '' || text === '—' || text === '-' || text === 'N/A') {
          validationIssues.push({
            type: 'placeholder_content',
            severity: 'info',
            region: regionName,
            description: `Region shows placeholder: "${text}"`,
            rect: GJSON.rects[regionName]
          });
        }
        
        // Check coverage-based empty detection
        const content = renderedContent[regionName];
        if (content.actualBounds && GJSON.rects[regionName]) {
          const rect = GJSON.rects[regionName];
          const rectArea = rect[2] * rect[3];
          const contentArea = content.actualBounds.width * content.actualBounds.height;
          const coverage = (contentArea / rectArea) * 100;
          const threshold = COVERAGE_THRESHOLDS[regionName] || COVERAGE_THRESHOLDS.default;
          
          // Skip coverage check for naturally small text (1-2 chars like units)
          const isNaturallySmall = text.length <= 2 && text !== '';
          
          if (!isNaturallySmall && coverage < threshold) {
            // Downgrade to info if it's a legitimate low coverage (e.g., "4%" text)
            const severity = (text.length > 0 && text.length <= 4) ? 'info' : 'warning';
            validationIssues.push({
              type: 'low_coverage',
              severity: severity,
              region: regionName,
              description: `Region appears empty (${coverage.toFixed(1)}% coverage)`,
              rect: GJSON.rects[regionName]
            });
          }
        }
      }
    }
    
    // Check for content outside defined regions
    for (const [regionName, content] of Object.entries(renderedContent)) {
      // Special case for elements that know they're outside their region
      if (content.outsideRegion) {
        validationIssues.push({
          type: 'element_outside_region',
          severity: 'error',
          region: regionName,
          description: `${regionName} renders outside ${content.outsideRegion}`,
          rect: content.actualBounds ? [content.actualBounds.x, content.actualBounds.y, content.actualBounds.width, content.actualBounds.height] : null
        });
        continue;
      }
      
      if (content.actualBounds && GJSON.rects[regionName]) {
        const rect = GJSON.rects[regionName];
        if (!Array.isArray(rect) || rect.length < 4) continue;
        const [rx, ry, rw, rh] = rect;
        const {x, y, width, height} = content.actualBounds;
        
        // Only flag significant overflows (>2px) to avoid false positives from antialiasing
        const leftOverflow = rx - x;
        const topOverflow = ry - y;
        const rightOverflow = (x + width) - (rx + rw);
        const bottomOverflow = (y + height) - (ry + rh);
        
        if (leftOverflow > 2 || topOverflow > 2 || rightOverflow > 2 || bottomOverflow > 2) {
          const details = [];
          if (leftOverflow > 2) details.push(`left by ${leftOverflow.toFixed(1)}px`);
          if (topOverflow > 2) details.push(`top by ${topOverflow.toFixed(1)}px`);
          if (rightOverflow > 2) details.push(`right by ${rightOverflow.toFixed(1)}px`);
          if (bottomOverflow > 2) details.push(`bottom by ${bottomOverflow.toFixed(1)}px`);
          
          validationIssues.push({
            type: 'bounds_exceeded',
            severity: rightOverflow > 5 || bottomOverflow > 5 ? 'error' : 'warning',
            region: regionName,
            description: `Content exceeds bounds: ${details.join(', ')}`,
            rect: rect
          });
        }
      }
    }
    
    // These regions exist in geometry but aren't used in v2
    // Note: HEADER_TIME_CENTER is now used for time display when header_centered component is active
    // OUT_PRESSURE is actually used in some configurations, don't mark as unused
    const v2SpecificUnused = [];
    
    // Check for regions defined but not used in current variant
    const allDefinedRegions = Object.keys(GJSON.rects || {});
    const additionalUnused = allDefinedRegions.filter(r => 
      !expectedContent.has(r) && 
      !v2SpecificUnused.includes(r) &&
      !r.includes('LABEL_BOX') && !r.includes('_INNER') && !r.includes('_BADGE')
    );
    
    // Combine both lists
    const unusedRegions = [...v2SpecificUnused, ...additionalUnused];
    for (const region of unusedRegions) {
      if (GJSON.rects[region] && !renderedContent[region]) {
        validationIssues.push({
          type: 'unused_region',
          severity: 'info',
          region: region,
          description: `Region defined but not used in v2 layout`,
          rect: GJSON.rects[region]
        });
      }
    }
    
    // Check for missing data fields with better categorization
    if (missingDataFields.size > 0) {
      const fields = Array.from(missingDataFields).sort();
      const criticalFields = ['inside_temp_f', 'room_name'];
      const importantFields = ['outside_temp_f', 'inside_hum_pct', 'battery_percent'];
      
      const missingCritical = fields.filter(f => criticalFields.includes(f));
      const missingImportant = fields.filter(f => importantFields.includes(f));
      
      if (missingCritical.length > 0) {
        validationIssues.push({
          type: 'missing_critical_data',
          severity: 'error',
          region: 'data',
          description: `Missing critical data: ${missingCritical.join(', ')}`,
          rect: null,
          suggestion: 'These fields are essential for basic functionality'
        });
      }
      
      if (missingImportant.length > 0) {
        validationIssues.push({
          type: 'missing_important_data',
          severity: 'warning',
          region: 'data',
          description: `Missing important data: ${missingImportant.join(', ')}`,
          rect: null,
          suggestion: 'Consider adding fallback values or placeholder text'
        });
      }
      
      const others = fields.filter(f => !criticalFields.includes(f) && !importantFields.includes(f));
      if (others.length > 0) {
        validationIssues.push({
          type: 'missing_data',
          severity: 'info',
          region: 'data',
          description: `Missing optional data: ${others.join(', ')}`,
          rect: null,
          suggestion: 'Optional fields - display may work without them'
        });
      }
    }
    
    // Validate data ranges and formats
    validateDataRanges(lastData);
    
    // De-duplicate issues
    const seen = new Set();
    validationIssues = validationIssues.filter(issue => {
      const key = `${issue.type}_${issue.region}_${issue.severity}`;
      if (seen.has(key)) return false;
      seen.add(key);
      return true;
    });
    
    // Update validation UI
    updateValidationDisplay();
    
    // Expose updated issues for testing
    try {
      if (typeof window !== 'undefined') {
        window.validationIssues = validationIssues;
        window.renderedContent = renderedContent;
      }
    } catch(e) {}
  }
  
  function drawValidationOverlay() {
    // Suppress validation overlay during labels-only to reduce clutter
    // Draw validation overlay when enabled and there are issues
    if (labelsOnlyMode || !validationEnabled || validationIssues.length === 0) return;
    
    ctx.save();
    
    // Draw issue highlights (skip WEATHER_ICON entirely to avoid any perceived border)
    for (const issue of validationIssues) {
      if (issue.rect) {
        const [x, y, w, h] = issue.rect;
        
        // Skip drawing for regions that shouldn't show validation
        // Also skip outside metric regions since we're not validating them
        // Also skip collision issues that involve outside metric regions
        if (issue.region && (issue.region === 'WEATHER_ICON' || issue.region.includes('_INNER') || 
            issue.region.includes('_BADGE') || 
            issue.region.includes('LABEL_BOX') ||
            issue.region === 'OUT_PRESSURE' ||
            issue.region === 'OUT_HUMIDITY' || issue.region === 'OUT_WIND')) {
          continue;
        }
        
        // Set color based on severity
        let color;
        switch(issue.severity) {
          case 'critical': color = 'rgba(255, 68, 68, 0.3)'; break;
          case 'error': color = 'rgba(255, 136, 0, 0.3)'; break;
          case 'warning': color = 'rgba(255, 187, 0, 0.3)'; break;
          default: color = 'rgba(0, 136, 255, 0.3)';
        }
        
        // Draw filled rectangle for the issue area (but never for WEATHER_ICON)
        ctx.fillStyle = color;
        ctx.fillRect(x, y, w, h);
        
        // Draw border
        ctx.strokeStyle = color.replace('0.3)', '1)');
        ctx.lineWidth = 2;
        ctx.strokeRect(x, y, w, h);
        
        // For text overflow issues, show the actual text bounds
        if (issue.type === 'text_overflow' && renderedContent[issue.region]) {
          const content = renderedContent[issue.region];
          if (content.actualBounds) {
            const {x: tx, y: ty, width: tw, height: th} = content.actualBounds;
            ctx.strokeStyle = 'rgba(255, 0, 0, 0.8)';
            ctx.lineWidth = 1;
            ctx.setLineDash([2, 2]);
            ctx.strokeRect(tx, ty, tw, th);
            ctx.setLineDash([]);
          }
        }
      }
    }
    
    ctx.restore();
  }

  // Draw region metrics overlay showing text widths and overflow indicators
  function drawRegionMetricsOverlay() {
    if (!window.__showRegionMetrics && !window.__highlightOverflow) return;
    if (!GJSON || !GJSON.rects) return;

    ctx.save();
    
    const fontStack = 'Menlo, Consolas, "DM Mono", "Roboto Mono", monospace';
    
    for (const [name, rect] of Object.entries(GJSON.rects)) {
      // Skip internal helper regions
      if (name.includes('_INNER') || name.includes('_BADGE') || name.includes('LABEL_BOX')) {
        continue;
      }

      const [x, y, w, h] = rect;
      const content = renderedContent[name];
      
      if (content && content.text && content.fontSize) {
        // Calculate text metrics
        ctx.font = `${content.weight || 'normal'} ${content.fontSize}px ${fontStack}`;
        const textMetrics = ctx.measureText(content.text);
        const textWidth = textMetrics.width;
        const textHeight = textMetrics.actualBoundingBoxAscent + textMetrics.actualBoundingBoxDescent || content.fontSize * 1.2;
        
        const overflowX = textWidth - w;
        const overflowY = textHeight - h;
        const hasOverflow = overflowX > 0 || overflowY > 0;
        const utilizationX = (textWidth / w) * 100;

        // Highlight overflow regions
        if (window.__highlightOverflow && hasOverflow) {
          ctx.fillStyle = 'rgba(255, 0, 0, 0.15)';
          ctx.fillRect(x, y, w, h);
          ctx.strokeStyle = 'rgba(255, 0, 0, 0.8)';
          ctx.lineWidth = 2;
          ctx.strokeRect(x, y, w, h);
        }

        // Draw metrics labels
        if (window.__showRegionMetrics) {
          // Draw text width indicator bar
          const barHeight = 3;
          const barY = y + h - barHeight - 1;
          
          // Background bar (region width)
          ctx.fillStyle = 'rgba(100, 100, 100, 0.5)';
          ctx.fillRect(x, barY, w, barHeight);
          
          // Fill bar (text width, capped to region width for display)
          const fillWidth = Math.min(textWidth, w);
          ctx.fillStyle = hasOverflow ? 'rgba(255, 0, 0, 0.8)' : 
                         utilizationX > 90 ? 'rgba(255, 165, 0, 0.8)' : 
                         'rgba(0, 180, 0, 0.8)';
          ctx.fillRect(x, barY, fillWidth, barHeight);
          
          // Draw overflow extension
          if (overflowX > 0) {
            ctx.fillStyle = 'rgba(255, 0, 0, 0.6)';
            ctx.fillRect(x + w, barY, Math.min(overflowX, 20), barHeight);
          }
          
          // Draw compact metrics label
          const label = hasOverflow 
            ? `${Math.round(textWidth)}px (+${Math.round(overflowX)})`
            : `${Math.round(textWidth)}/${w}px`;
          
          ctx.font = '8px monospace';
          ctx.fillStyle = hasOverflow ? '#c00' : '#333';
          ctx.textAlign = 'left';
          ctx.textBaseline = 'bottom';
          ctx.fillText(label, x + 1, barY - 1);
        }
      }
    }
    
    ctx.restore();
    
    // Store geometry globally for metrics system
    window.__geometry = GJSON;
  }

  async function loadCentralGeometry(){
    // Try window.UI_SPEC first (if embedded)
    try{
      if (typeof window !== 'undefined' && window.UI_SPEC){
        const gj = window.UI_SPEC;
        if (gj && gj.rects){
          applyGeometry(gj);
          console.log('✅ Loaded geometry from window.UI_SPEC');
          return;
        }
      }
    }catch(e){
      console.warn('Failed to load from window.UI_SPEC:', e);
    }

    // Load from geometry.json file
    try{
      const res = await fetch('geometry.json');
      if (res.ok){
        const gj = await res.json();
        if (gj && gj.rects){
          applyGeometry(gj);
          console.log('✅ Loaded geometry from geometry.json', gj.layout_crc ? `(CRC: ${gj.layout_crc})` : '');
          return;
        }
      }
      throw new Error('geometry.json missing or invalid');
    }catch(e){
      console.error('❌ CRITICAL: Failed to load geometry.json - simulator cannot render!', e);
      throw e;
    }
  }

  function applyGeometry(gj){
    GJSON = gj;
    window.GJSON = gj; // Expose globally for layout editor
    WIDTH = (gj.canvas && gj.canvas.w) || WIDTH;
    HEIGHT = (gj.canvas && gj.canvas.h) || HEIGHT;
    const R = gj.rects;
    
    // Validate rects object exists before accessing properties
    if (!R || typeof R !== 'object') {
      throw new Error('Geometry file missing rects object');
    }

    // Apply all regions (no fallbacks - fail if missing)
    HEADER_NAME = R.HEADER_NAME;
    HEADER_VERSION = R.HEADER_VERSION;
    HEADER_TIME_CENTER = R.HEADER_TIME_CENTER;
    INSIDE_TEMP = R.INSIDE_TEMP;
    INSIDE_HUMIDITY = R.INSIDE_HUMIDITY;
    INSIDE_PRESSURE = R.INSIDE_PRESSURE;
    OUT_TEMP = R.OUT_TEMP;
    WEATHER_ICON = R.WEATHER_ICON;
    OUT_PRESSURE = R.OUT_PRESSURE;
    OUT_HUMIDITY = R.OUT_HUMIDITY;
    OUT_WIND = R.OUT_WIND;

    // Validate critical regions loaded
    const required = ['HEADER_NAME', 'INSIDE_TEMP', 'OUT_TEMP', 'WEATHER_ICON'];
    const missing = required.filter(name => !R[name]);
    if (missing.length > 0) {
      throw new Error(`Missing required regions: ${missing.join(', ')}`);
    }
  }

  function applyOneBitThreshold(){
    if (!oneBitMode) return;
    const img = ctx.getImageData(0,0,WIDTH,HEIGHT);
    const d = img.data;
    for (let i=0;i<d.length;i+=4){
      const r=d[i], g=d[i+1], b=d[i+2];
      // Tri-color aware: the panel has three inks, so clearly-red pixels snap
      // to the red ink (#cc0000) instead of being crushed by the luminance
      // threshold; everything else quantizes black/white as before.
      if (r > 120 && r > g * 2 && r > b * 2){
        d[i]=204; d[i+1]=0; d[i+2]=0; d[i+3]=255;
        continue;
      }
      const y = 0.2126*r + 0.7152*g + 0.0722*b;
      const v = y < THRESH ? 0 : 255;
      d[i]=d[i+1]=d[i+2]=v;
      d[i+3]=255;
    }
    ctx.putImageData(img,0,0);
  }

  function drawGridOverlay(){
    if (!showGrid || labelsOnlyMode) return;
    ctx.save();
    ctx.strokeStyle = '#bbb';
    ctx.lineWidth = 1;
    for (let x = 0; x <= WIDTH; x += 4){
      ctx.beginPath();
      ctx.moveTo(x + 0.5, 0);
      ctx.lineTo(x + 0.5, HEIGHT);
      ctx.stroke();
    }
    for (let y = 0; y <= HEIGHT; y += 4){
      ctx.beginPath();
      ctx.moveTo(0, y + 0.5);
      ctx.lineTo(WIDTH, y + 0.5);
      ctx.stroke();
    }
    ctx.restore();
  }

  function categoryColor(cat){
    switch(String(cat||'')){
      case 'header': return '#ff6b6b';
      case 'temp':   return '#4ecdc4';
      case 'label':  return '#45b7d1';
      case 'footer': return '#96ceb4';
      default:       return '#f00';
    }
  }

  function hexToRgba(hex, alpha){
    try{
      const h = String(hex||'').trim();
      if (!/^#?[0-9a-fA-F]{6}$/.test(h)) return `rgba(255,0,0,${alpha||0.15})`;
      const s = h.startsWith('#') ? h.slice(1) : h;
      const n = parseInt(s, 16);
      const r = (n >> 16) & 255;
      const g = (n >> 8) & 255;
      const b = (n) & 255;
      const a = (typeof alpha === 'number') ? alpha : 0.15;
      return `rgba(${r}, ${g}, ${b}, ${a})`;
    }catch(e){ return `rgba(255,0,0,${alpha||0.15})`; }
  }

  function drawRectsOverlay(){
    if (!showRects || !GJSON || !GJSON.rects) return;
    ctx.save();
    // Fill rects with a translucent color so misalignment is obvious
    ctx.lineWidth = 1;
    Object.entries(GJSON.rects).forEach(([name, r])=>{
      if (!shouldShowRect(name)) return;
      // Skip internal helper rectangles - these are implementation details
      if (name.includes('_INNER') || name.includes('_BADGE') || name.includes('LABEL_BOX')) {
        return;
      }
      // Allow WEATHER_ICON to render in overlay so it can be positioned visually
      if (searchQuery){
        const q = searchQuery.toLowerCase();
        if (!String(name).toLowerCase().includes(q)) return;
      }
      const [x,y,w,h] = r;
      const cat = getRectCategory(name);
      const col = categoryColor(cat);
      // Semi-transparent fill distinguishable from content (skip threshold when overlays are active)
      ctx.fillStyle = hexToRgba(col, 0.15);
      ctx.strokeStyle = (highlightRect === name) ? '#000' : col;
      ctx.lineWidth = (highlightRect === name) ? 2 : 1;
      ctx.fillRect(x, y, w, h);
      ctx.strokeRect(x+0.5, y+0.5, w, h);
      if (showLabels){
        ctx.fillStyle = '#000';
        ctx.font = `bold 10px ${FONT_STACK}`;
        ctx.textBaseline = 'top';
        let label = String(name);
        if (name === 'WEATHER_ICON') {
          label = 'ICON';
        } else if (w < 50 && label.length > 8) {
          label = label.substring(0, 6) + '..';
        }
        ctx.fillText(label, x+2, y+2);
      }
    });
    // Distinguish chrome/border lines by drawing them in green in overlay mode
    try {
      if (!labelsOnlyMode){
      const spec = (typeof window !== 'undefined') ? window.UI_SPEC : null;
      const chrome = (spec && spec.components && spec.components.chrome) ? spec.components.chrome : [];
      ctx.strokeStyle = '#0a0';
      ctx.lineWidth = 2;
      chrome.forEach(op=>{
        if (op && op.op === 'line' && Array.isArray(op.from) && Array.isArray(op.to)){
          let fx = (op.from[0]|0);
          let fy = (op.from[1]|0);
          let tx = (op.to[0]|0);
          let ty = (op.to[1]|0);
          // Snap v2_grid overlay chrome to spec positions (header y=18, divider x=125 from 18..121, footer y=84)
          try{
            if (typeof window !== 'undefined' && window.__specMode === 'v2_grid'){
              if (fy === 16 && ty === 16) { fy = ty = 18; }
              if (fx === 128 && tx === 128) { fx = tx = 125; if (fy === 16) fy = 18; }
              if (fy === 88 && ty === 88) { fy = ty = 84; }
            }
          }catch(e){}
          fx += 0.5; fy += 0.5; tx += 0.5; ty += 0.5;
          ctx.beginPath();
          ctx.moveTo(fx, fy);
          ctx.lineTo(tx, ty);
          ctx.stroke();
        }
      });
      // Also outline the full canvas as a green border
      ctx.strokeRect(0.5, 0.5, WIDTH-1, HEIGHT-1);
      // no extra issue highlighter
      }
    } catch(e){}
    ctx.restore();
  }

  function getRectCategory(name){
    const n = String(name||'');
    if (n.startsWith('HEADER_')) return 'header';
    if (n.startsWith('FOOTER_')) return 'footer';
    if (/_LABEL_BOX$/.test(n)) return 'label';
    if (n === 'WEATHER_ICON') return 'label';
    if (n.startsWith('OUT_')) return 'temp';
    if (/_TEMP(|_INNER|_BADGE)?$/.test(n) || n === 'INSIDE_HUMIDITY' || n === 'INSIDE_PRESSURE') return 'temp';
    return 'temp';
  }

  function shouldShowRect(name){
    if (!regionFilters.all){
      const cat = getRectCategory(name);
      if (!regionFilters[cat]) return false;
    }
    if (regionSelectionActive){
      return regionVisible.has(name);
    }
    return true;
  }

  function saveRegionPrefs(){
    try{
      const suffix = storageSuffix();
      localStorage.setItem(`sim_region_filters::${suffix}`, JSON.stringify(regionFilters));
      localStorage.setItem(`sim_region_visible::${suffix}`, JSON.stringify(Array.from(regionVisible)));
      localStorage.setItem(`sim_region_selection_active::${suffix}`, JSON.stringify(!!regionSelectionActive));
    }catch(e){}
  }

  function loadRegionPrefs(){
    try{
      const suffix = storageSuffix();
      const f = JSON.parse(localStorage.getItem(`sim_region_filters::${suffix}`)||'null');
      if (f && typeof f === 'object') regionFilters = { ...regionFilters, ...f };
      const v = JSON.parse(localStorage.getItem(`sim_region_visible::${suffix}`)||'null');
      if (Array.isArray(v)) regionVisible = new Set(v);
      const a = JSON.parse(localStorage.getItem(`sim_region_selection_active::${suffix}`)||'null');
      if (typeof a === 'boolean') regionSelectionActive = a;
    }catch(e){}
  }

  function storageSuffix(){
    try{
      const variant = (QS.get('variant') || (window.UI_SPEC && window.UI_SPEC.defaultVariant) || 'v1');
      const specMode = (typeof window !== 'undefined' && window.__specMode) ? String(window.__specMode) : 'v1';
      return `${variant}::${specMode}`;
    }catch(e){ return 'v1::v1'; }
  }

  function refreshRegionList(){
    const listEl = document.getElementById('regionList');
    if (!listEl || !GJSON || !GJSON.rects) return;
    listEl.innerHTML = '';
    const names = Object.keys(GJSON.rects).sort();
    names.forEach(name=>{
      // Skip internal helper rectangles from the list
      if (name.includes('_INNER') || name.includes('_BADGE') || name.includes('LABEL_BOX')) {
        return;
      }
      
      const cat = getRectCategory(name);
      const wrap = document.createElement('label');
      wrap.style.display = (regionFilters.all || regionFilters[cat]) ? 'block' : 'none';
      wrap.className = 'region-item';
      wrap.dataset.region = name;
      wrap.addEventListener('mouseenter', ()=>{ highlightRect = name; scheduleDraw(); });
      wrap.addEventListener('mouseleave', ()=>{ if (highlightRect === name) { highlightRect = null; scheduleDraw(); }});
      const cb = document.createElement('input');
      cb.type = 'checkbox';
      cb.className = 'region-checkbox';
      cb.value = name;
      cb.checked = regionSelectionActive ? regionVisible.has(name) : true;
      cb.addEventListener('change', ()=>{
        // Ensure overlays are visible when interacting with region list
        showRects = true; const rectsEl = document.getElementById('showRects'); if (rectsEl) rectsEl.checked = true;
        // Initialize selection with all names on first change so unchecking works immediately
        if (!regionSelectionActive){ regionSelectionActive = true; regionVisible = new Set(names); }
        if (cb.checked){ regionVisible.add(name); } else { regionVisible.delete(name); }
        saveRegionPrefs(); scheduleDraw();
      });
      const txt = document.createElement('span');
      txt.textContent = `${name} (${cat})`;
      wrap.appendChild(cb); wrap.appendChild(txt);
      listEl.appendChild(wrap);
    });
  }

  function scheduleDraw(){
    if (pendingDraw) return;
    pendingDraw = requestAnimationFrame(()=>{ pendingDraw = 0; draw({}); });
  }

  function text(x,y,str,size=10,weight='normal',regionName=null,color=null){
    ctx.fillStyle = color || '#000';
    ctx.font = `${weight} ${size}px ${FONT_STACK}`;
    ctx.textBaseline = 'top';
    ctx.fillText(str, x, y);
    
    // Track rendered content for validation
    if (regionName && validationEnabled) {
      const metrics = ctx.measureText(str);
      renderedContent[regionName] = {
        text: str,
        fontSize: size,
        weight: weight,
        actualBounds: {
          x: x,
          y: y,
          width: metrics.width,
          height: size
        }
      };
    }
  }

  // Evaluates a spec op's `when` clause. Only `has(<field>)` exists today, and
  // gen_ui.py rejects any other form so the firmware and this renderer cannot
  // diverge. DEFAULTS counts as a source, matching the sim's value lookup below.
  //
  // "Has a value" has to mean the same thing here as in the firmware's
  // spec_field_has(), which requires isfinite() on the reading. So a numeric field
  // that arrives as NaN/Infinity, as an empty string, or as one of Home
  // Assistant's unavailable sentinels counts as absent - otherwise the sim would
  // render a row the device leaves blank. Non-numeric fields (a weather string,
  // say) only have to be non-empty.
  const UNAVAILABLE = ['', 'nan', 'none', 'null', 'unavailable', 'unknown'];
  function specFieldHasValue(v){
    if (v === undefined || v === null) return false;
    const s = String(v).trim();
    if (UNAVAILABLE.includes(s.toLowerCase())) return false;
    const n = Number(s);
    // Number('cloudy') is NaN, i.e. not numeric at all: keep those as present.
    return Number.isNaN(n) ? true : Number.isFinite(n);
  }
  function specFieldHas(when, data){
    const whenStr = String(when || '');
    if (!(whenStr.startsWith('has(') && whenStr.endsWith(')'))) return true;
    const field = whenStr.slice(4, -1).trim();
    if (data && specFieldHasValue(data[field])) return true;
    return (typeof window !== 'undefined' && window.DEFAULTS)
      ? specFieldHasValue(window.DEFAULTS[field])
      : false;
  }

  console.log('Defining drawFromSpec function...');
  function drawFromSpec(ctx, data, variantName){
    try{
      const spec = (typeof window !== 'undefined' && window.UI_SPEC) ? window.UI_SPEC : {};
      const rects = spec.rects || {};
      const fonts = (spec.fonts && spec.fonts.tokens) ? spec.fonts.tokens : {};
      const pxSmall = (fonts.small && fonts.small.px) ? fonts.small.px : SIZE_SMALL;
      const pxLabel = (fonts.label && fonts.label.px) ? fonts.label.px : SIZE_LABEL;
      const pxTime = (fonts.time && fonts.time.px) ? fonts.time.px : SIZE_TIME;
      const variants = spec.variants || {};
      const list = variants[variantName || spec.defaultVariant] || [];
      // Export layout metrics for tests
      window.__layoutMetrics = { labels: {}, weather: {}, statusLeft: {} };
      window.__tempMetrics = { inside: {}, outside: {} };
      // Shared {placeholder} expander for text-bearing ops, mirroring the
      // firmware's spec_expand_template/spec_format_field pair. One expander,
      // not per-op copies - the textCenteredIn mini-expander drifted once
      // (no format specs, no DEFAULTS fallback, decimal temps).
      const expandTemplate = (raw) => String(raw || '').replace(/\{([^}]+)\}/g, (_, k) => {
        if (k === 'fw_version'){
          if (data.fw_version !== undefined && data.fw_version !== null && String(data.fw_version) !== '') return String(data.fw_version);
          if (typeof window !== 'undefined' && typeof window.UI_FW_VERSION === 'string') return window.UI_FW_VERSION;
        }
        if (k === 'weather_short') return shortConditionLabel(data.weather || '');
        const base = k.replace(/[:].*$/, '').replace(/->.*$/, '');
        let val = (data[base] !== undefined && data[base] !== null)
          ? data[base]
          : ((typeof window !== 'undefined' && window.DEFAULTS) ? window.DEFAULTS[base] : undefined);
        if (val === undefined || val === null) {
          const alias = base.replace(/_f$/, '');
          val = (data[alias] !== undefined && data[alias] !== null)
            ? data[alias]
            : ((typeof window !== 'undefined' && window.DEFAULTS) ? window.DEFAULTS[alias] : undefined);
        }
        if (val === undefined || val === null) {
          if (validationEnabled) missingDataFields.add(base);
          return '';
        }
        const conv = k.match(/->([a-z]+)/);
        if (conv){
          const to = conv[1]; const num = parseFloat(String(val));
          if (isFinite(num) && to === 'mph') val = (num * 2.237);
        }
        const fmt = k.match(/:(.*)$/);
        if (fmt){
          const m = fmt[1].match(/\.(\d)f/);
          if (m){
            const d = parseInt(m[1]);
            if (!isNaN(d) && d >= 0 && d <= 20) {
              const num = parseFloat(String(val));
              if (isFinite(num)) val = num.toFixed(d);
            }
          }
        } else if (base === 'inside_temp_f' || base === 'outside_temp_f') {
          // Firmware spec_format_field renders these with %.0f - integer
          // degrees on the glass, so decimal feeds must not widen sim text.
          const num = parseFloat(String(val));
          if (isFinite(num)) val = String(Math.round(num));
        }
        return String(val);
      });
      for (const cname of list){
        const ops = (spec.components || {})[cname] || [];
        for (const op of ops){
          try{
          // Spec `when: has(<field>)` guard - skip the op when the field has no
          // value. Applies to every op kind, matching spec_field_has() in the
          // firmware (firmware/arduino/src/main.cpp).
          if (op.when && !specFieldHas(op.when, data)) continue;
          switch(op.op){
            case 'sparkline': {
              // 24h history polyline. Series pairs sharing a rect share a scale
              // (group = hist_temp_* or hist_rh_*) so in/out are comparable.
              const r = rects[op.rect]; if (!r) break;
              const arr = data[op.series]; if (!Array.isArray(arr) || arr.length < 1) break;
              const groupKeys = String(op.series).startsWith('hist_temp')
                ? ['hist_temp_in','hist_temp_out'] : ['hist_rh_in','hist_rh_out'];
              let mn=Infinity, mx=-Infinity;
              for (const gk of groupKeys){
                const a = data[gk]; if (!Array.isArray(a)) continue;
                for (const v of a){ if (isFinite(v)) { if (v<mn) mn=v; if (v>mx) mx=v; } }
              }
              if (!isFinite(mn) || mx===mn) { mx = mn+1; }
              const cap = 288;  // 24h of 5-min samples; 'now' anchors right
              ctx.strokeStyle = (op.color === 'red') ? '#cc0000' : '#000'; ctx.lineWidth=1;
              ctx.setLineDash(op.style==='dashed' ? [2,2] : []);
              ctx.beginPath();
              let started=false;
              for (let i=0;i<arr.length;i++){
                const v=arr[i]; if (!isFinite(v)) { started=false; continue; }
                const px = r[0] + (r[2]-1) - (arr.length-1-i)*(r[2]-1)/(cap-1);
                const py = r[1] + (r[3]-1) - (v-mn)/(mx-mn)*(r[3]-1);
                if (!started){ ctx.moveTo(px,py); started=true; } else ctx.lineTo(px,py);
              }
              ctx.stroke(); ctx.setLineDash([]);
              // Sparse-data dots, mirroring the firmware (main.cpp OP_SPARKLINE):
              // on the fixed 24h axis a young ring is a sub-pixel stub, so mark
              // each sample until the polyline can carry the chart.
              if (arr.length < 48) {
                ctx.fillStyle = (op.color === 'red') ? '#cc0000' : '#000';
                for (let i=0;i<arr.length;i++){
                  const v=arr[i]; if (!isFinite(v)) continue;
                  const px = Math.min(r[0] + (r[2]-1) - (arr.length-1-i)*(r[2]-1)/(cap-1), r[0]+r[2]-2);
                  const py = Math.min(r[1] + (r[3]-1) - (v-mn)/(mx-mn)*(r[3]-1), r[1]+r[3]-2);
                  ctx.fillRect(px, py, 2, 2);
                }
              }
              // Plot frame stays black regardless of the series' line color,
              // matching the device (its frame is hard-coded GxEPD_BLACK).
              ctx.strokeStyle = '#000';
              ctx.strokeRect(r[0]-1, r[1]-1, r[2]+2, r[3]+2);
              break;
            }
            case 'fill': {
              // Solid rect fill (inverted regions). Matches firmware OP_FILL.
              // 'red' is the tri-color wing's accent; mono panels draw black.
              const r = rects[op.rect]; if (!r) break;
              ctx.fillStyle = (op.color === 'white') ? '#fff' : (op.color === 'red') ? '#cc0000' : '#000';
              ctx.fillRect(r[0], r[1], r[2], r[3]);
              break;
            }
            case 'frame': {
              // 1px outline (value chips). Four fillRects, not strokeRect,
              // so the 1-bit canvas stays free of antialiased grays.
              const r = rects[op.rect]; if (!r) break;
              ctx.fillStyle = (op.color === 'red') ? '#cc0000' : '#000';
              ctx.fillRect(r[0], r[1], r[2], 1);
              ctx.fillRect(r[0], r[1] + r[3] - 1, r[2], 1);
              ctx.fillRect(r[0], r[1], 1, r[3]);
              ctx.fillRect(r[0] + r[2] - 1, r[1], 1, r[3]);
              break;
            }
            case 'line': {
              const fx = (op.from && op.from[0]) || 0;
              const fy = (op.from && op.from[1]) || 0;
              const tx = (op.to && op.to[0]) || 0;
              const ty = (op.to && op.to[1]) || 0;
              ctx.fillStyle = '#000';
              if (fy === ty) ctx.fillRect(Math.min(fx,tx), fy, Math.abs(tx - fx) + 1, 1);
              else if (fx === tx) ctx.fillRect(fx, Math.min(fy,ty), 1, Math.abs(ty - fy) + 1);
              break;
            }
            case 'text': {
              const r = op.rect ? rects[op.rect] : null;
              const fpx = ((fonts[op.font||'small']||{}).px) || pxSmall;
              const weight = ((fonts[op.font||'small']||{}).weight) || 'normal';
              let s = expandTemplate(op.text);
              if (r){
                // Pad clipping box slightly to avoid cutting off glyph ascenders/descenders
                const __pad_top = 1, __pad_bottom = 1, __pad_left = 0, __pad_right = 0;
                ctx.save();
                ctx.beginPath();
                ctx.rect(r[0] - __pad_left, r[1] - __pad_top, r[2] + __pad_left + __pad_right, r[3] + __pad_top + __pad_bottom);
                ctx.clip();
                
                // Apply smart truncation if specified
                if (op.truncate === 'ellipsis') {
                  const maxW = r[2] - 4; // Leave 2px padding on each side
                  s = smartTruncate(s, maxW, fpx, weight);
                }
                
                // Alignment within rect if provided
                let x;
                ctx.font = `${weight} ${fpx}px ${FONT_STACK}`; ctx.textBaseline='top';
                if (op.align === 'right') {
                  const tw = ctx.measureText(s).width;
                  x = r[0] + r[2] - 2 - tw;
                } else if (op.align === 'center') {
                  const tw = ctx.measureText(s).width;
                  x = r[0] + Math.max(0, Math.floor((r[2] - tw)/2));
                } else {
                  // Add 1px padding for left-aligned text to avoid touching the edge
                  x = (op.x !== undefined) ? (r[0] + op.x) : (r[0] + 1);
                }
                // Add 1px padding from top for better appearance
                const y = (op.y !== undefined) ? (r[1] + op.y) : (r[1] + 1);
                // Use our text function for tracking
                text(x, y, s, fpx, weight, op.rect, (op.color === 'inverse') ? '#fff' : (op.color === 'red' || op.color === 'red-inverse') ? '#cc0000' : null);
                // Export footer metrics for layout tests, keyed by rect
                if (op.rect === 'FOOTER_BATTERY'){
                  const textW = ctx.measureText(s).width;
                  window.__layoutMetrics.statusLeft.line1Y = y;
                  window.__layoutMetrics.statusLeft.left = r[0];
                  window.__layoutMetrics.statusLeft.right = r[0] + r[2];
                  window.__layoutMetrics.statusLeft.batteryText = { x, w: textW };
                } else if (op.rect === 'FOOTER_IP'){
                  const textW = ctx.measureText(s).width;
                  window.__layoutMetrics.statusLeft.ip = { x, w: textW };
                }
                ctx.restore();
              } else {
                const x = op.x||0; const y = op.y||0;
                // Handle maxWidth and truncate for absolute positioned text
                ctx.font = `${weight} ${fpx}px ${FONT_STACK}`;
                ctx.textBaseline = 'top';
                ctx.fillStyle = '#000';
                
                if (op.maxWidth && op.truncate === 'ellipsis') {
                  const maxW = op.maxWidth;
                  // Use smart truncation for better word-break handling
                  s = smartTruncate(s, maxW, fpx, weight);
                  // Clip to maxWidth to ensure nothing overflows
                  ctx.save();
                  ctx.beginPath();
                  ctx.rect(x, y, maxW, fpx + 4);
                  ctx.clip();
                  // Use the actual rect name for tracking
                  const regionName = op.rect;
                  text(x, y, s, fpx, weight, regionName);
                  ctx.restore();
                } else {
                  // Use the actual rect name for tracking
                  const regionName = op.rect;
                  text(x, y, s, fpx, weight, regionName);
                }
                
              }
              break;
            }
            // 'timeRight' removed - no longer in ui_spec.json (v2's header uses
            // textCenteredIn); firmware dropped OP_TIMERIGHT the same day.
            case 'labelCentered': {
              const r = rects[op.aboveRect]; if (!r) break;
              const fpx = ((fonts[op.font||'label']||{}).px) || pxLabel;
              const weight = ((fonts[op.font||'label']||{}).weight) || 'bold';
              const lab = String(op.text||'');
              ctx.font = `${weight} ${fpx}px ${FONT_STACK}`; ctx.textBaseline='top'; ctx.fillStyle='#000';
              const lw = ctx.measureText(lab).width;
              // Position label above the temperature region
              const lx = r[0] + Math.floor((r[2] - lw)/2);
              const ly = op.y || 24; // Use specified y or default to 24
              text(lx, ly, lab, fpx, weight, op.aboveRect ? op.aboveRect + '_LABEL' : undefined);
              if (op.aboveRect === 'INSIDE_TEMP') window.__layoutMetrics.labels.inside = { x: lx + lw/2 };
              if (op.aboveRect === 'OUT_TEMP') window.__layoutMetrics.labels.outside = { x: lx + lw/2 };
              break;
            }
            case 'tempGroupCentered': {
              // Big temperature + units, centered in the op's rect. The metrics
              // key comes from the op's value template (inside_temp_f /
              // outside_temp_f), not from rect-name matching - variants can name
              // rects anything. (INNER/BADGE rect lookups removed: no spec has
              // ever defined them, so those branches never ran.)
              const r = rects[op.rect]; if (!r) break;
              const fontSize = SIZE_BIG;
              ctx.font = `bold ${fontSize}px ${FONT_STACK}`; ctx.textBaseline='top';
              const field = String(op.value||'').replace(/[{}]/g,'');
              const s = String(data[field] ?? '');
              const unitsW = 14;
              const tw = ctx.measureText(s).width;
              const totalW = Math.min(Math.max(0, r[2]-2), tw + unitsW);
              const left = Math.round(r[0] + Math.max(0, Math.floor((r[2] - totalW)/2)));
              const yTop = Math.round(r[1] + Math.max(0, Math.floor(((r[3]||28) - fontSize) / 2)));
              // color:"red" renders the digits + units in the tri-color accent
              const digitColor = (op.color === 'red') ? '#cc0000' : null;
              text(left, yTop, s, fontSize, 'bold', op.rect, digitColor);
              const unitSize = 12, unitYOffset = 3;
              text(left + tw + 2, yTop + unitYOffset, '°', unitSize, 'normal', undefined, digitColor);
              text(left + tw + 8, yTop + unitYOffset, 'F', unitSize, 'normal', undefined, digitColor);
              const key = field.startsWith('inside') ? 'inside'
                        : (field.startsWith('outside') ? 'outside' : null);
              if (key){ window.__tempMetrics[key] = { rect: { x: r[0], y: r[1], w: r[2], h: (r[3]||0) }, contentLeft: left, totalW: (tw + unitsW) }; }
              break;
            }
            case 'textCenteredIn': {
              const r = rects[op.rect]; if (!r) break;
              const fpx = ((fonts[op.font||'small']||{}).px) || pxSmall;
              const weight = ((fonts[op.font||'small']||{}).weight) || 'normal';
              const raw = String(op.text||'');
              const s = expandTemplate(raw);
              ctx.font = `${weight} ${fpx}px ${FONT_STACK}`; ctx.textBaseline='top';
              const tw = ctx.measureText(s).width;
              // Center text horizontally in rect (matches firmware behavior)
              const x = r[0] + Math.max(0, Math.floor((r[2]-tw)/2));
              const yTop = (op.yOffset? (r[1]+op.yOffset) : r[1]);
              text(x, yTop, s, fpx, weight, op.rect, (op.color === 'inverse') ? '#fff' : (op.color === 'red' || op.color === 'red-inverse') ? '#cc0000' : null);
              if (raw.includes('IP ')){
                window.__layoutMetrics.statusLeft.ip = { x, w: tw };
              }
              break;
            }
            case 'iconIn': {
              // Icon only - the condition label is always its own text op, in
              // every variant, matching the firmware exactly. The old "legacy"
              // icon+label fallback and its isV2 gating produced two real bugs
              // (doubled labels) and served no spec: deleted.
              const r = rects[op.rect]; if (!r) break;
              if (validationEnabled) {
                renderedContent[op.rect] = {
                  text: 'weather_icon', fontSize: 0,
                  actualBounds: { x: r[0], y: r[1], width: r[2], height: r[3] }
                };
              }
              const barX = r[0], barY = r[1], barW = r[2], barH = r[3];
              // Clear the icon area, clamped inside the 1px display border.
              const clearX0 = Math.max(1, barX - 2);
              const clearY0 = Math.max(1, barY - 2);
              const clearX1 = Math.min(WIDTH - 1, barX + barW + 2);
              const clearY1 = Math.min(HEIGHT - 1, barY + barH + 2);
              ctx.fillStyle = '#fff';
              ctx.fillRect(clearX0, clearY0, Math.max(0, clearX1 - clearX0), Math.max(0, clearY1 - clearY0));
              const inset = 2;
              ctx.save();
              ctx.beginPath();
              ctx.rect(barX + inset, barY + inset, Math.max(0, barW - inset*2), Math.max(0, barH - inset*2));
              ctx.clip();
              const iconW = Math.max(14, (barW - inset*2));
              const iconH = Math.max(12, (barH - inset*2));
              const startX = barX + inset;
              const startY = barY + inset;
              ctx.strokeStyle = '#000'; ctx.fillStyle = '#000';
              const category = classifyWeather(data.weather);
              if (!tryDrawBakedBitmap(category, startX, startY, iconW, iconH)){
                const drewSvg = tryDrawMdiIcon(category, startX, startY, iconW, iconH);
                if (!drewSvg) drawWeatherGlyph(category, startX, startY, iconW, iconH);
              }
              ctx.restore();
              if (op.color === 'red'){
                // Recolor the glyph's dark pixels to the red ink, mirroring the
                // device passing map_op_color(p3) into draw_icon().
                const img = ctx.getImageData(barX, barY, barW, barH);
                const d = img.data;
                for (let i = 0; i < d.length; i += 4){
                  if (d[i] < 128 && d[i+1] < 128 && d[i+2] < 128){
                    d[i] = 204; d[i+1] = 0; d[i+2] = 0;
                  }
                }
                ctx.putImageData(img, barX, barY);
              }
              break;
            }
            case 'batteryGlyph': {
              // Support both direct coordinates and rect-based positioning
              let x, y, bw = op.w || 13, bh = op.h || 7;
              if (op.rect && rects[op.rect]) {
                const r = rects[op.rect];
                // Position battery icon at left of rect, vertically centered
                x = r[0] + 2;
                y = r[1] + Math.floor((r[3] - bh) / 2);
              } else {
                x = op.x || 0;
                y = op.y || 0;
              }
              // Prefer battery_percent; fall back to op.percent template or 0
              let pct = 0;
              try{
                const tpl = String(op.percent||'').replace(/[{}]/g,'');
                const val = (data[tpl]!==undefined) ? data[tpl] : data.battery_percent;
                pct = parseInt(String(val||0), 10);
              }catch(e){ pct = parseInt(String(data.battery_percent||0), 10); }
              // Draw crisp 1px border via filled rectangles to avoid any stroke artifacts
              ctx.fillStyle = '#000';
              // Top border
              ctx.fillRect(x, y, bw, 1);
              // Bottom border
              ctx.fillRect(x, y + bh - 1, bw, 1);
              // Left border
              ctx.fillRect(x, y, 1, bh);
              // Right border
              ctx.fillRect(x + bw - 1, y, 1, bh);
              // Terminal nub
              ctx.fillRect(x + bw, y + 2, 2, 3);
              // Inner fill based on percentage
              const fillw = Math.max(0, Math.min(bw-2, Math.round((bw-2) * (pct/100))));
              if (fillw > 0) ctx.fillRect(x+1, y+1, fillw, bh-2);
              window.__layoutMetrics.statusLeft.batteryIcon = { x, y, w: bw, h: bh };
              
              // Track battery icon for validation
              if (validationEnabled) {
                // Just track that we rendered the battery icon
                const trackRect = op.rect || 'FOOTER_BATTERY';
                renderedContent[trackRect] = {
                  text: 'battery',
                  fontSize: 0,
                  actualBounds: { x, y, width: bw + 2, height: bh }
                };
                // Also mark FOOTER_STATUS as having content for backwards compat
                if (!renderedContent['FOOTER_STATUS'] && !op.rect) {
                  renderedContent['FOOTER_STATUS'] = {
                    text: 'battery_area',
                    fontSize: 0,
                    actualBounds: { x, y, width: bw + 2, height: bh }
                  };
                }
              }
              break;
            }
            default: break;
          }
          }catch(err){
            try{ console.error('drawFromSpec op failed:', (op&&op.op), (op&&op.rect), err); }catch(_){ }
            // Continue rendering remaining ops/components
          }
        }
      }
    }catch(e){ }
  }
  // Ensure global exposure
  if (typeof window !== 'undefined') {
    window.drawFromSpec = drawFromSpec;
  }

  const __demoHist = (()=>{
    const N=288, ti=[], to=[], ri=[], ro=[];
    for (let i=0;i<N;i++){
      const t=i/N*2*Math.PI;
      ti.push(73+1.6*Math.sin(t-2)+0.6*Math.sin(9*t));
      to.push(72+14*Math.sin(t-2.4));
      ri.push(36+5*Math.sin(t+0.6));
      ro.push(60-22*Math.sin(t-2.4));
    }
    return {ti,to,ri,ro};
  })();
  // Variant picker (C2): populated from the spec so new variants appear
  // automatically; selection overrides for the session and updates the URL.
  try {
    const sel = document.getElementById('variantSel');
    if (sel && window.UI_SPEC && window.UI_SPEC.variants) {
      const names = Object.keys(window.UI_SPEC.variants);
      const current = QS.get('variant') || window.UI_SPEC.defaultVariant || names[0];
      for (const n of names) {
        const o = document.createElement('option');
        o.value = n; o.textContent = n + (n === window.UI_SPEC.defaultVariant ? ' (default)' : '');
        if (n === current) o.selected = true;
        sel.appendChild(o);
      }
      sel.addEventListener('change', () => {
        window.__variantOverride = sel.value;
        const url = new URL(window.location.href);
        url.searchParams.set('variant', sel.value);
        window.history.replaceState(null, '', url.toString());
        if (typeof window.draw === 'function') window.draw();
        else if (typeof draw === 'function') draw();
      });
    }
  } catch(e) { console.warn('variant picker init failed', e); }

  const DEFAULTS = {
    room_name: 'Office',
    hist_temp_in: __demoHist.ti, hist_temp_out: __demoHist.to,
    hist_rh_in: __demoHist.ri, hist_rh_out: __demoHist.ro,
    hist_temp_min: 58, hist_temp_max: 86, hist_rh_min: 31, hist_rh_max: 82,
    time_hhmm: '10:32',
    date_mmmdd: 'Jul 30',
    inside_temp_f: 72.5,
    inside_hum_pct: 47,
    outside_temp_f: 68.4,
    outside_hum_pct: 53,
    // Provide outside pressure so OUT_PRESSURE renders by default
    outside_pressure_hpa: 1016,
    weather: 'cloudy',
    wind_mph: 4.2,
    wind_mps: 1.88,  // Add default wind_mps value
    battery_percent: 76,
    battery_voltage: 4.01,
    days: '128',
    ip: '192.168.1.42',
    pressure_hpa: 1013.2
  };
  let lastData = { ...DEFAULTS };
  try{ 
    if (typeof window !== 'undefined') {
      window.lastData = lastData;
      window.DEFAULTS = DEFAULTS;  // Expose defaults for debug panel
    }
  }catch(e){}
  
  // Expose validation for testing
  try {
    if (typeof window !== 'undefined') {
      window.validationIssues = validationIssues;
      window.renderedContent = renderedContent;
      window.runValidation = runValidation;
      window.drawValidationOverlay = drawValidationOverlay;
    }
  } catch(e) {}
  
  // Export validation results with optional screenshot
  window.exportValidation = function(opts = { includeScreenshot: false }) {
    const VALIDATION_VERSION = "1.0.0";
    
    // Ensure fresh validation results
    if (typeof window.runValidation === 'function') {
      window.runValidation();
    }
    
    // Map issues to stable format
    let issues = (window.validationIssues || []).map(i => ({
      type: i.type,
      severity: i.severity,
      region: i.region,
      description: i.description,
      rect: i.rect || null,
      suggestion: i.suggestion || null
    }));
    
    // Sort issues deterministically
    const severityOrder = { critical: 0, error: 1, warning: 2, info: 3 };
    issues.sort((a, b) => {
      const sevDiff = severityOrder[a.severity] - severityOrder[b.severity];
      if (sevDiff !== 0) return sevDiff;
      
      const regionDiff = (a.region || '').localeCompare(b.region || '');
      if (regionDiff !== 0) return regionDiff;
      
      return (a.type || '').localeCompare(b.type || '');
    });
    
    // Optionally include screenshot
    let screenshot = null;
    if (opts.includeScreenshot && window.canvas && window.canvas.toDataURL) {
      screenshot = window.canvas.toDataURL('image/png');
    }
    
    // Return structured result
    return {
      issues: issues,
      screenshot: screenshot,
      variant: (window.QS && window.QS.get('variant')) || 'v2_grid',
      timestamp: window.__lastDrawAt || null,
      ready: window.__simReady || false,
      validationVersion: VALIDATION_VERSION
    };
  };

  function draw(data){
    console.log('draw() called with data:', data);
    const __start = (typeof performance !== 'undefined' && performance.now) ? performance.now() : Date.now();
    
    // Update state manager with new data
    if (data && simDataState) {
      simDataState.update(data);
    }
    
    // Ensure canvas is initialized
    if (!ctx) {
      // Try to initialize canvas if not ready yet
      if (!initCanvas()) {
        console.log('Canvas not ready yet, deferring draw');
        return;
      }
    }
    console.log('Canvas context available');
    
    // Clear rendered content tracking for validation
    renderedContent = {};
    emptyRegions.clear();
    missingDataFields.clear();
    
    // Merge with defaults and existing data
    if (data && typeof data === 'object' && Object.keys(data).length) {
      // Merge new data with existing lastData
      lastData = { ...lastData, ...data };
    }
    try{ if (typeof window !== 'undefined') window.lastData = lastData; }catch(e){}
    // Render via spec only
    const variant = (typeof window!=='undefined' && window.__variantOverride)
      || QS.get('variant')
      || (typeof window!=='undefined' && window.UI_SPEC && window.UI_SPEC.defaultVariant) || 'v2';
    
    ctx.fillStyle = '#fff'; ctx.fillRect(0,0,WIDTH,HEIGHT);
    
    console.log('geometryOnly:', geometryOnly, 'drawFromSpec exists:', typeof window.drawFromSpec === 'function');
    if (!geometryOnly && typeof window !== 'undefined' && typeof window.drawFromSpec === 'function'){
      console.log('Calling drawFromSpec');
      window.drawFromSpec(ctx, lastData, variant);
    } else {
      console.log('Skipping drawFromSpec - geometryOnly:', geometryOnly);
    }
    
    // Redraw chrome on top to ensure continuous lines (header, divider, footer)
    // Skip lines that would conflict with labels (y=18-31)
    try{
      const spec = (typeof window !== 'undefined') ? window.UI_SPEC : null;
      const chrome = (spec && spec.components && spec.components.chrome) ? spec.components.chrome : [];
      ctx.fillStyle = '#000';
      chrome.forEach(op=>{
        if (op && op.op === 'line' && Array.isArray(op.from) && Array.isArray(op.to)){
          const fx = (op.from[0]|0);
          const fy = (op.from[1]|0);
          const tx = (op.to[0]|0);
          const ty = (op.to[1]|0);
          // Skip horizontal lines in the label area (y=18-31)
          if (fy === ty && fy >= 18 && fy <= 31) return;
          if (fy === ty) ctx.fillRect(Math.min(fx,tx), fy, Math.abs(tx - fx) + 1, 1);
          else if (fx === tx) ctx.fillRect(fx, Math.min(fy,ty), 1, Math.abs(ty - fy) + 1);
        }
      });
    }catch(e){}
    
    // Apply 1-bit threshold BEFORE overlays for consistent rendering
    // This ensures text is properly thresholded before any debug overlays are drawn
    if (oneBitMode && !geometryOnly) {
      applyOneBitThreshold();
    }
    
    // Run validation after drawing content but before overlays
    runValidation();
    
    // Draw overlays only if enabled (these are drawn on top of the thresholded image)
    drawGridOverlay();
    // In labels-only, force showLabels=true and showRects=true temporarily for overlay
    const prevShowLabels = showLabels;
    const prevShowRects = showRects;
    if (labelsOnlyMode){ showLabels = true; showRects = true; }
    drawRectsOverlay();
    if (labelsOnlyMode){ showLabels = prevShowLabels; showRects = prevShowRects; }
    
    // Draw validation overlay (it checks its own conditions internally)
    drawValidationOverlay();

    // Draw region metrics overlay if enabled
    drawRegionMetricsOverlay();

    // Draw layout editor overlay (interactive editing)
    if (typeof window !== 'undefined' && window.layoutEditor && window.layoutEditor.drawOverlay) {
      window.layoutEditor.drawOverlay(ctx);
    }

    // Draw debug overlay last (on top of everything)
    if (typeof window !== 'undefined' && window.DebugOverlay && window.DebugOverlay.enabled) {
      window.DebugOverlay.drawRegionBounds(ctx);
      if (window.__renderTimings) {
        window.DebugOverlay.drawRenderTiming(ctx, window.__renderTimings);
      }
      window.DebugOverlay.highlightDataFlow(ctx);
    }

    // Leave some tokens for tests to find in sim.js
    // weather-sunny weather-partly-cloudy weather-cloudy weather-fog
    // weather-pouring weather-snowy weather-lightning weather-night
    // weather-night-partly-cloudy weather-windy-variant
    // Layout constants
    // DISPLAY_WIDTH DISPLAY_HEIGHT RECT_HEADER_NAME RECT_OUT_TEMP CANVAS
    
    // Mark simulator as ready after draw completes
    window.__simReady = true;
    window.__lastDrawAt = Date.now();
    if (simulateGhosting){
      // Enhanced eInk ghosting simulation
      const img = ctx.getImageData(0,0,WIDTH,HEIGHT);
      const d = img.data;
      
      // Multiple ghosting effects for realism
      for (let i=0;i<d.length;i+=4){
        const x = (i/4) % WIDTH;
        const y = Math.floor((i/4) / WIDTH);
        
        // 1. Previous image retention (more pronounced near edges)
        if (d[i] === 255){
          const edgeDist = Math.min(x, WIDTH-x, y, HEIGHT-y);
          const edgeFactor = edgeDist < 20 ? 0.15 : 0.05;
          if (Math.random() < edgeFactor) {
            const ghost = 255 - Math.floor(Math.random() * 35 + 10);
            d[i]=d[i+1]=d[i+2]=ghost;
          }
        }
        
        // 2. Partial refresh artifacts (horizontal bands)
        if (y % 32 === 0 || y % 32 === 31) { // Refresh boundaries
          if (d[i] === 255 && Math.random() < 0.1) {
            d[i]=d[i+1]=d[i+2]=240;
          }
        }
        
        // 3. Voltage-dependent contrast variation
        const contrastNoise = Math.sin(x * 0.1) * Math.cos(y * 0.1) * 10;
        if (d[i] === 0) {
          d[i]=d[i+1]=d[i+2]=Math.max(0, Math.min(30, Math.floor(contrastNoise)));
        }
      }
      
      ctx.putImageData(img,0,0);
      applyOneBitThreshold();
    }
    // Update status UI and timing data
    try {
      const renderMs = (typeof performance !== 'undefined' && performance.now) ? (performance.now() - __start) : (Date.now() - window.__lastDrawAt);
      window.__lastRenderMs = renderMs;

      // Store detailed timing for debug overlay
      if (!window.__renderTimings) window.__renderTimings = {};
      window.__renderTimings.lastFrameMs = renderMs;
      window.__renderTimings.timestamp = Date.now();
      window.__renderTimings.fps = renderMs > 0 ? (1000 / renderMs) : 0;

      if (typeof updateStatusUI === 'function') updateStatusUI(renderMs);
    } catch (e) {}
  }
  
  // Expose draw function to window for debug panel
  if (typeof window !== 'undefined') {
    window.draw = draw;
  }

  // Canvas click → toggle labels-only when clicking a label region
  (function setupCanvasClicks(){
    try{
      const c = document.getElementById('epd');
      if (!c) return;
      c.addEventListener('click', (ev)=>{
        // Skip this handler if layout editor is enabled - it handles its own clicks
        if (window.layoutEditor && window.layoutEditor.state && window.layoutEditor.state.enabled) {
          return;
        }
        if (!GJSON || !GJSON.rects) return;
        const rect = c.getBoundingClientRect();
        const zoom = Math.max(1, parseFloat(getComputedStyle(document.documentElement).getPropertyValue('--zoom')) || 2);
        const cx = Math.floor((ev.clientX - rect.left) / zoom);
        const cy = Math.floor((ev.clientY - rect.top) / zoom);
        // Find top-most matching region under cursor, preferring label regions
        let clickedName = null;
        for (const [name, r] of Object.entries(GJSON.rects)){
          if (name.includes('_INNER') || name.includes('_BADGE')) continue;
          const [x,y,w,h] = r;
          if (cx >= x && cx <= x+w && cy >= y && cy <= y+h){
            clickedName = name;
            // Prefer label boxes when overlapping
            if (/_LABEL_BOX$/.test(name) || name === 'WEATHER_ICON') break;
          }
        }
        if (!clickedName) return;
        const isLabelRegion = /_LABEL_BOX$/.test(clickedName) || clickedName === 'WEATHER_ICON';
        if (isLabelRegion){
          labelsOnlyMode = !labelsOnlyMode;
          // When entering labels-only: hide normal content and show only labels
          if (labelsOnlyMode){
            geometryOnly = true; // skip drawFromSpec content
            showRects = true; // ensure rect anchors visible (but overlay suppresses fills)
            showLabels = true; // ensure labels visible
            const rectsEl = document.getElementById('showRects'); if (rectsEl) rectsEl.checked = true;
            const labelsEl = document.getElementById('showLabels'); if (labelsEl) labelsEl.checked = true;
          } else {
            geometryOnly = false;
          }
          draw(lastData);
        }
      });
    }catch(e){}
  })();

  // sample_data.json nests scenarios ({default:{...}, time_of_day:{...}});
  // tests may route a flat payload instead — accept both shapes. Returns a
  // shallow copy so callers can annotate it without mutating the source.
  function unwrapSamplePayload(payload){
    if (payload && typeof payload === 'object' && payload.default) return { ...payload.default };
    return payload;
  }

  async function load(){
    console.log('load() called');
    
    // Try to initialize canvas (it's OK if it fails, draw() will retry)
    if (!initCanvas()) {
      console.log('Canvas not ready during initial load');
    }
    
    loadRegionPrefs();
    await loadCentralGeometry();
    
    console.log('Calling draw with lastData:', lastData);
    draw(lastData);
    try{
      const gres = await fetch('geometry.json?v=2');
      if (gres.ok){ GEOMETRY = await gres.json(); }
    }catch(e){ GEOMETRY = null; }
    try{
      const res = await fetch('sample_data.json');
      if(!res.ok) throw new Error('fetch failed');
      const data = unwrapSamplePayload(await res.json());
      draw(data);
    } catch(e){ }
    // Wire region inspector controls
    try{
      const fAll = document.getElementById('filterAll');
      const fHeader = document.getElementById('filterHeader');
      const fTemp = document.getElementById('filterTemp');
      const fLabel = document.getElementById('filterLabel');
      const fFooter = document.getElementById('filterFooter');
      const resetBtn = document.getElementById('resetRegionFilters');
      const normalizeAll = ()=>{
        // Computed: checked when all categories are checked
        regionFilters.all = !!(regionFilters.header && regionFilters.temp && regionFilters.label && regionFilters.footer);
        if (fAll) fAll.checked = regionFilters.all;
      };
      const apply = ()=>{ showRects = true; const rEl = document.getElementById('showRects'); if (rEl) rEl.checked = true; normalizeAll(); saveRegionPrefs(); refreshRegionList(); scheduleDraw(); };
      if (fAll){ fAll.checked = !!regionFilters.all; /* disabled in HTML; readonly */ }
      if (fHeader){ fHeader.checked = !!regionFilters.header; fHeader.addEventListener('change', ()=>{ regionFilters.header = !!fHeader.checked; apply(); }); }
      if (fTemp){ fTemp.checked = !!regionFilters.temp; fTemp.addEventListener('change', ()=>{ regionFilters.temp = !!fTemp.checked; apply(); }); }
      if (fLabel){ fLabel.checked = !!regionFilters.label; fLabel.addEventListener('change', ()=>{ regionFilters.label = !!fLabel.checked; apply(); }); }
      if (fFooter){ fFooter.checked = !!regionFilters.footer; fFooter.addEventListener('change', ()=>{ regionFilters.footer = !!fFooter.checked; apply(); }); }
      if (resetBtn){ resetBtn.addEventListener('click', ()=>{
        regionFilters = { all: true, header: true, temp: true, label: true, footer: true };
        regionVisible = new Set(); regionSelectionActive = false;
        apply();
      }); }
      // Bulk actions
      const showAllBtn = document.getElementById('showAllRects');
      const hideAllBtn = document.getElementById('hideAllRects');
      if (showAllBtn){ showAllBtn.addEventListener('click', ()=>{
        regionSelectionActive = true;
        regionVisible = new Set(Object.keys(GJSON.rects||{}));
        saveRegionPrefs(); refreshRegionList(); scheduleDraw();
      }); }
      if (hideAllBtn){ hideAllBtn.addEventListener('click', ()=>{
        regionSelectionActive = true;
        regionVisible = new Set();
        saveRegionPrefs(); refreshRegionList(); scheduleDraw();
      }); }
      // Search
      const searchEl = document.getElementById('regionSearch');
      if (searchEl){ searchEl.addEventListener('input', ()=>{ searchQuery = String(searchEl.value||''); refreshRegionList(); scheduleDraw(); }); }
      refreshRegionList();
    }catch(e){}
  }

  const refreshEl = document.getElementById('refresh');
  if (refreshEl) {
    refreshEl.addEventListener('click', async ()=>{
      try{
        const res = await fetch('sample_data.json');
        const data = unwrapSamplePayload(await res.json());
        data.time = new Date().toTimeString().slice(0,5);
        lastData = data;
        // Partial redraw demo: clear header version rect and re-render spec variant
        const [hx,hy,hw,hh] = HEADER_VERSION;
        ctx.fillStyle = '#fff'; ctx.fillRect(hx,hy,hw,hh);
        const variant = QS.get('variant') || (window.UI_SPEC && window.UI_SPEC.defaultVariant) || 'v2';
        if (typeof window !== 'undefined' && typeof window.drawFromSpec === 'function'){
          window.drawFromSpec(ctx, lastData, variant);
        }
        applyOneBitThreshold();
        // Stamp the redraw so test settle-waits see this refresh (the direct
        // drawFromSpec path bypasses draw(), which normally sets this).
        window.__lastDrawAt = Date.now();
      }catch(e){ load(); }
    });
  }
  // Zoom control
  function setZoom(z){
    const zoom = Math.max(1, Math.min(4, Number(z) || 2));
    try { localStorage.setItem('simZoom', String(zoom)); } catch(e){}
    const root = document.documentElement;
    if (root && root.style) root.style.setProperty('--zoom', zoom);
    const zVal = document.getElementById('zoomValue'); if (zVal) zVal.textContent = `${zoom}×`;
  }
  const zoomEl = document.getElementById('zoom');
  if (zoomEl){
    const storedZ = (function(){ try { return parseFloat(localStorage.getItem('simZoom')||'2'); } catch(e){ return 2; } })();
    if (!isNaN(storedZ)) { zoomEl.value = String(storedZ); setZoom(storedZ); }
    zoomEl.addEventListener('input', (e)=>{ setZoom(e.target.value); });
  }

  // Screenshot download
  const shotBtn = document.getElementById('screenshot');
  if (shotBtn){
    shotBtn.addEventListener('click', ()=>{
      try{
        const canvas = document.getElementById('epd');
        if (!canvas) return;
        const url = canvas.toDataURL('image/png');
        const a = document.createElement('a');
        a.href = url; a.download = `sim-${Date.now()}.png`; a.click();
        showGlobalToast('Downloaded PNG');
      }catch(e){ console.error('screenshot failed', e); }
    });
  }
  // Screenshot copy to clipboard
  const copyBtn = document.getElementById('copyShot');
  if (copyBtn){
    function showGlobalToast(msg){ 
      const g=document.getElementById('globalToast'); 
      if(!g) return;
      g.textContent = msg; 
      g.classList.add('show'); 
      setTimeout(()=>{ g.classList.remove('show'); }, 1500); 
    }
    
    copyBtn.addEventListener('click', async ()=>{
      try{
        const canvas = document.getElementById('epd'); 
        if (!canvas) return;
        
        // Try Safari-compatible clipboard API first
        if (window.ClipboardItem && navigator.clipboard && navigator.clipboard.write) {
          try {
            showGlobalToast('Copying...');
            // Safari fix: Pass promise directly without awaiting
            const blobPromise = new Promise((resolve) => {
              canvas.toBlob((blob) => resolve(blob), 'image/png');
            });
            
            await navigator.clipboard.write([
              new ClipboardItem({
                'image/png': blobPromise
              })
            ]);
            showGlobalToast('Copied to clipboard');
            return;
          } catch(err) {
            // Clipboard API failed, fall through to modal
          }
        }
        
        // Fallback: Show copyable image in modal
        const url = canvas.toDataURL('image/png');
        
        // Remove any existing modal
        const existingModal = document.getElementById('copyModal');
        if (existingModal) existingModal.remove();
        
        // Create modal
        const modal = document.createElement('div');
        modal.id = 'copyModal';
        modal.style.cssText = `
          position: fixed;
          top: 0;
          left: 0;
          width: 100%;
          height: 100%;
          background: rgba(0,0,0,0.8);
          display: flex;
          align-items: center;
          justify-content: center;
          z-index: 10000;
          cursor: pointer;
        `;
        
        const container = document.createElement('div');
        container.style.cssText = `
          background: white;
          padding: 20px;
          border-radius: 8px;
          text-align: center;
          max-width: 90%;
          max-height: 90%;
          overflow: auto;
        `;
        container.onclick = (e) => e.stopPropagation();
        
        const instructions = document.createElement('p');
        instructions.textContent = 'Right-click the image below and select "Copy Image"';
        instructions.style.cssText = 'margin-bottom: 10px; font-family: system-ui;';
        container.appendChild(instructions);
        
        const img = document.createElement('img');
        img.src = url;
        img.style.cssText = 'max-width: 100%; border: 1px solid #ccc;';
        img.alt = 'ESP32 Display Screenshot';
        container.appendChild(img);
        
        const closeBtn = document.createElement('button');
        closeBtn.textContent = 'Close';
        closeBtn.style.cssText = `
          margin-top: 10px;
          padding: 8px 16px;
          background: #4B5563;
          color: white;
          border: none;
          border-radius: 4px;
          cursor: pointer;
          font-family: system-ui;
        `;
        closeBtn.onclick = () => modal.remove();
        container.appendChild(closeBtn);
        
        modal.onclick = () => modal.remove();
        modal.appendChild(container);
        document.body.appendChild(modal);
        
        showGlobalToast('Right-click image to copy');
      }catch(e){ 
        showGlobalToast('Copy failed - try Screenshot button'); 
      }
    });
  }

  // Reset UI button
  const resetBtn = document.getElementById('resetUI');
  if (resetBtn){
    resetBtn.addEventListener('click', ()=>{
      // Reset toggles
      const ids = ['showWindows','stressMode','showGrid','showRects','showLabels','simulateGhosting'];
      ids.forEach(id=>{ const el = document.getElementById(id); if (el){ el.checked = false; el.dispatchEvent(new Event('change')); }});
      // Reset selects
      const presetSel = document.getElementById('presetMode'); if (presetSel){ presetSel.value = 'normal'; presetSel.dispatchEvent(new Event('change')); }
      // Reset zoom
      setZoom(2); if (zoomEl) zoomEl.value = '2';
      // Exit labels-only mode if active
      labelsOnlyMode = false;
      geometryOnly = false;
      draw(lastData);
      // Clear persisted panel open states
      try{
        const suffix = (function(){
          try{
            const variant = (QS.get('variant') || (window.UI_SPEC && window.UI_SPEC.defaultVariant) || 'v1');
            const specMode = (typeof window !== 'undefined' && window.__specMode) ? String(window.__specMode) : 'v1';
            return `${variant}::${specMode}`;
          }catch(e){ return 'v1::v1'; }
        })();
        ['regionInspector','validationPanel','advancedDebugDetails'].forEach(id=>{
          try{ localStorage.removeItem(`sim_panel_open::${id}::${suffix}`); }catch(e){}
        });
      }catch(e){}
      // Redraw
      draw(window.lastData || {});
    });
  }

  // Status bar/pill updates
  function updateStatusUI(renderMs){
    try{
      const ready = !!window.__simReady;
      const pill = document.getElementById('statusPill');
      if (pill){
        pill.textContent = ready ? 'Ready' : 'Not Ready';
        pill.classList.remove('ok','warn');
        pill.classList.add(ready ? 'ok' : 'warn');
      }
      const bar = document.getElementById('statusBar');
      if (bar){
        const issuesCount = (Array.isArray(validationIssues) ? validationIssues.length : 0);
        bar.textContent = `${ready ? '✓ Simulator ready' : '… Initializing'} | Issues: ${issuesCount}`;
      }
    }catch(e){}
  }
  
  const showWindowsEl = document.getElementById('showWindows');
  if (showWindowsEl) {
    showWindowsEl.addEventListener('change', (e)=>{
      showWindows = !!e.target.checked; draw({});
    });
  }
  
  // Stress mode toggle with baseline restore
  let __baselineData = null;
  const stressModeEl = document.getElementById('stressMode');
  if (stressModeEl) {
    stressModeEl.addEventListener('change', (e)=>{
      stressMode = !!e.target.checked;
      if (stressMode && !__baselineData) {
        try { __baselineData = { ...lastData }; } catch(_) { __baselineData = null; }
      }
      if (stressMode) {
        const stress = {
          room_name: 'Extremely Long Room Name Example',
          time_hhmm: '23:59',
          inside_temp_f: 99.9,
          inside_hum_pct: 100,
          outside_temp_f: -40.0,
          outside_hum_pct: 0,
          pressure_hpa: 1085.0,
          wind_mph: 99.9,
          weather: 'thunderstorm with hail and extreme winds',
          battery_percent: 3,
          battery_voltage: 3.20,
          days: 999,
          ip: '10.1.2.3'
        };
        draw(stress);
      } else {
        if (__baselineData) {
          // Restore snapshot and redraw without merging
          lastData = { ...__baselineData };
          try{ window.lastData = lastData; }catch(_){ }
          draw({});
        } else {
          draw({});
        }
        __baselineData = null;
      }
    });
  }
  const gridEl = document.getElementById('showGrid');
  if (gridEl) gridEl.addEventListener('change', (e)=>{ showGrid = !!e.target.checked; draw({}); });
  const rectsEl = document.getElementById('showRects');
  if (rectsEl) rectsEl.addEventListener('change', (e)=>{ 
    showRects = !!e.target.checked; 
    // Force a full redraw with current data to clear any artifacts
    draw(lastData); 
  });
  const labelsEl = document.getElementById('showLabels');
  if (labelsEl) labelsEl.addEventListener('change', (e)=>{ 
    showLabels = !!e.target.checked; 
    // When labeling, ensure rects are visible so labels have anchors
    if (showLabels){ 
      showRects = true; 
      const rectsEl = document.getElementById('showRects'); 
      if (rectsEl) rectsEl.checked = true; 
      // Enter labels-only mode when user enables label regions from the toolbar
      labelsOnlyMode = true; 
      geometryOnly = true; 
    }
    // Do NOT toggle geometryOnly here; labels should overlay on full render
    // Force a full redraw to clear any artifacts
    // If labels-only was active and user explicitly toggled labels, exit labels-only
    if (!showLabels && labelsOnlyMode){ labelsOnlyMode = false; geometryOnly = false; }
    draw(lastData); 
  });
  const ghostEl = document.getElementById('simulateGhosting');
  if (ghostEl) ghostEl.addEventListener('change', (e)=>{ simulateGhosting = !!e.target.checked; draw({}); });
  // removed highlightIssues wiring
  // Spec-only control removed from UI; rendering uses spec by design
  const variantSel = document.getElementById('variantMode');
  if (variantSel){
    // Hide the variant selector since it causes confusion - we only use the Layout Version selector
    if (variantSel.parentElement && variantSel.parentElement.tagName === 'LABEL') {
      variantSel.parentElement.style.display = 'none';
    }
    // Set variant based on specMode (only force v2_grid when explicitly requested)
    const __specModeParam = (QS.get('specMode') || '').toLowerCase();
    if (__specModeParam === 'v2_grid') {
    variantSel.value = 'v2_grid';
    } else {
      variantSel.value = (typeof window !== 'undefined' && window.UI_SPEC && window.UI_SPEC.defaultVariant) || 'v2';
    }
    
  }
  // Presets
  const presetSel = document.getElementById('presetMode');
  if (presetSel){
    presetSel.addEventListener('change', ()=>{
      const preset = presetSel.value || 'normal';
      const base = { ...DEFAULTS };
      switch(preset){
        case 'low_batt':
          base.battery_percent = 12;
          base.battery_voltage = 3.42;
          break;
        case 'no_mqtt':
          // Just use the single v2 variant
          // Data will show as missing if not provided
          base.weather = '';
          base.outside_temp_f = '';
          base.outside_hum_pct = '';
          break;
        case 'extreme':
          base.inside_temp_f = -10.2;
          base.outside_temp_f = 109.9;
          base.outside_hum_pct = 5;
          base.wind_mph = 99.9;
          break;
        case 'long_city':
          base.room_name = 'Extremely Long Room Name Example That Wraps';
          break;
        default:
          // normal
          break;
      }
      draw(base);
    });
  }
  // Initialize optional specMode override (e.g., ?specMode=v2_grid)
  {
    const which = (QS.get('specMode') || '').toLowerCase();
    try{
      if (which === 'v2_grid'){
          // Construct a v2 spec by cloning UI_SPEC and snapping rects + fonts
          const base = JSON.parse(JSON.stringify(window.UI_SPEC || {}));
          if (!base.rects) base.rects = {};
          // Define a clean 4px-grid layout with 12px outer padding and 4px gutters
          const OUTER = 12;
          const DIV_X = 128; // vertical divider aligned to grid
          const HEADER_Y = 4, HEADER_H = 12; // top rule at y=16
          const TEMP_Y = 32, TEMP_H = 28; // Moved down to avoid label collision (was 24)
          // Adjust rows and footer (aligned to 4px grid)
          const ROW1_Y = 60;  // Was 52, adjusted for TEMP_Y change
          const ROW2_Y = 72;  // Was 68, adjusted for TEMP_Y change
          const ROW_H = 12;
          // Footer divider line stays at y=90 to match UI spec chrome, footer regions start at y=92
          const FOOTER_LINE_Y = 90;
          const FOOTER_Y = 92;
          const FOOTER_H = 28;
          const LEFT_X = OUTER; const LEFT_W = DIV_X - OUTER - 4; // 12..124 -> 112 (4px gap before divider)
          const RIGHT_X = DIV_X + 4; const RIGHT_W = 250 - OUTER - RIGHT_X; // from 132 -> 106

          // Prevent header overlaps in v2 grid: narrow left/name and shift time
          base.rects.HEADER_NAME = [LEFT_X, HEADER_Y, 84, HEADER_H];
          base.rects.HEADER_TIME_CENTER = [100, HEADER_Y, 48, HEADER_H];
          base.rects.HEADER_VERSION = [152, HEADER_Y, 88, HEADER_H];

          base.rects.INSIDE_TEMP = [LEFT_X, TEMP_Y, LEFT_W, TEMP_H];
          // Inner number area leaves margin for units; taller to prevent cropping
          const innerY = TEMP_Y + 4;
          const innerH = TEMP_H - 4; // 24px
          base.rects.INSIDE_TEMP_INNER = [LEFT_X + 4, innerY, LEFT_W - 20, innerH];
          base.rects.INSIDE_TEMP_BADGE = [LEFT_X + LEFT_W - 16, innerY, 12, 12];
          // Inside rows: keep pressure in same spot; move RH slightly down (handled by rows below)
          base.rects.INSIDE_HUMIDITY   = [LEFT_X, ROW1_Y + 2, LEFT_W, ROW_H];
          base.rects.INSIDE_PRESSURE = [LEFT_X, ROW2_Y, LEFT_W, ROW_H];

          base.rects.OUT_TEMP    = [RIGHT_X, TEMP_Y, RIGHT_W, TEMP_H];
          // Match inner adjustments on the right side
          base.rects.OUT_TEMP_INNER = [RIGHT_X + 4, innerY, RIGHT_W - 20, innerH];
          base.rects.OUT_TEMP_BADGE = [RIGHT_X + RIGHT_W - 16, innerY, 12, 12];
          // Re-layout right column quadrants: RH top-left, wind top-right, pressure bottom-left, bottom-right blank
          base.rects.OUT_HUMIDITY = [RIGHT_X, ROW1_Y + 2, 48, ROW_H];
          base.rects.OUT_WIND     = [RIGHT_X + 52, ROW1_Y + 2, 48, ROW_H];
          base.rects.OUT_PRESSURE = [RIGHT_X, ROW2_Y, 54, ROW_H];
          // bottom-right blank intentionally (no rect assigned)

          // Bottom-right quadrant: from RIGHT_X to right outer margin
          const BR_X = RIGHT_X;
          const BR_RIGHT = 250 - OUTER;
          const BR_W = Math.max(0, BR_RIGHT - BR_X);
          // Larger left-anchored icon and label to the right within the quadrant
          const ICON_INSET = 2;
          const ICON_W = Math.min(40, Math.max(20, Math.floor(BR_W * 0.36)));
          base.rects.WEATHER_ICON = [BR_X + ICON_INSET, FOOTER_Y, ICON_W, FOOTER_H];

          // Footer columns - extend FOOTER_STATUS 10px to the left to keep content clear of boundaries
          base.rects.FOOTER_STATUS    = [Math.max(0, LEFT_X - 10), FOOTER_Y, LEFT_W + 10, FOOTER_H];
          // Allocate remaining space to FOOTER_WEATHER (label) with a small gap
          const GAP = 6;
          const LABEL_X = BR_X + ICON_INSET + ICON_W + GAP;
          const LABEL_W = Math.max(0, BR_RIGHT - LABEL_X);
          base.rects.FOOTER_WEATHER    = [LABEL_X, FOOTER_Y, LABEL_W, FOOTER_H];

          // Adjust chrome lines to match grid
          if (base.components && Array.isArray(base.components.chrome)){
            // Honor UI spec chrome lines, but ensure header (y=18) and footer (y=90) lines are present
            base.components.chrome = [
              { op: 'line', from: [0, 0],   to: [249, 0] },
              { op: 'line', from: [0, 121], to: [249, 121] },
              { op: 'line', from: [0, 0],   to: [0, 121] },
              { op: 'line', from: [249, 0], to: [249, 121] },
              { op: 'line', from: [125, 18], to: [125, 121] },
              { op: 'line', from: [1, 18], to: [249, 18] },
              { op: 'line', from: [1, FOOTER_LINE_Y], to: [249, FOOTER_LINE_Y] }
            ];
          }
          // Adjust fonts
          if (!base.fonts) base.fonts = {};
          if (!base.fonts.tokens) base.fonts.tokens = {};
          base.fonts.tokens.big = { px: 22, weight: 'bold' };
          base.fonts.tokens.label = { px: 12, weight: 'bold' };
          base.fonts.tokens.small = { px: 10 };
          base.fonts.tokens.time = { px: 10 };
          // Replace global UI_SPEC at runtime for sim display only
          window.UI_SPEC = base;
          // Re-apply central geometry
          GJSON = base;
          window.__specMode = which;
          // Normalize region selection to current rect set
          try{
            const names = new Set(Object.keys(GJSON.rects||{}));
            if (regionSelectionActive){
              const newSel = new Set();
              regionVisible.forEach(n=>{ if (names.has(n)) newSel.add(n); });
              regionVisible = newSel;
              if (regionVisible.size === 0) regionSelectionActive = false;
            }
          }catch(e){}
      }
    }catch(e){}
    refreshRegionList();
    draw({});
  }
  // Validation panel controls
  const enableValidationCheckbox = document.getElementById('enableValidation');
  const validationOverlayToggle = document.getElementById('toggleValidationOverlay');
  // Initialize default state: validation disabled
  if (enableValidationCheckbox) enableValidationCheckbox.checked = false;
  if (validationOverlayToggle) validationOverlayToggle.checked = false;
  const syncValidationUI = ()=>{
    if (enableValidationCheckbox && enableValidationCheckbox.checked !== validationEnabled) {
      enableValidationCheckbox.checked = validationEnabled;
    }
    if (validationOverlayToggle && validationOverlayToggle.checked !== validationEnabled) {
      validationOverlayToggle.checked = validationEnabled;
    }
  };
  const onValidationToggle = (enabled)=>{
    validationEnabled = !!enabled;
    if (validationEnabled) {
      runValidation();
      // Ensure overlays render immediately
      try { draw(window.lastData || {}); } catch(_) { draw({}); }
    } else {
      validationIssues = [];
      updateValidationDisplay();
      // Also force redraw to clear overlay artifacts
      try { draw(window.lastData || {}); } catch(_) { draw({}); }
    }
    syncValidationUI();
  };
  if (enableValidationCheckbox) {
    enableValidationCheckbox.addEventListener('change', (e) => {
      onValidationToggle(e.target.checked);
    });
  }
  if (validationOverlayToggle) {
    validationOverlayToggle.addEventListener('change', (e) => {
      onValidationToggle(e.target.checked);
    });
  }
  
  const runValidationBtn = document.getElementById('runValidation');
  if (runValidationBtn) {
    runValidationBtn.addEventListener('click', () => {
      onValidationToggle(true);
    });
  }
  
  const clearValidationBtn = document.getElementById('clearValidation');
  if (clearValidationBtn) {
    clearValidationBtn.addEventListener('click', () => {
      validationIssues = [];
      updateValidationDisplay();
      // Keep validation disabled after clear to match intent
      onValidationToggle(false);
    });
  }
  
  // Function to apply validation fixes
  function applyValidationFix(fixType, region) {
    console.log(`Applying fix: ${fixType} to region: ${region}`);
    
    if (fixType === 'truncate') {
      // Find the data field for this region and truncate it
      if (region === 'HEADER_NAME' && lastData.room_name) {
        const maxLen = 15; // Reasonable max for header
        if (lastData.room_name.length > maxLen) {
          lastData.room_name = lastData.room_name.substring(0, maxLen - 3) + '...';
          draw(lastData);
          debugLog(`Truncated room name to fit`, 'success');
        }
      }
    } else if (fixType === 'smaller_font') {
      // This would need firmware changes, so just log suggestion
      debugLog(`Font size reduction would require firmware update for ${region}`, 'info');
    }
  }
  
  // Helper function for debug logging
  function debugLog(message, type = 'info') {
    const debugConsole = document.getElementById('debugConsole');
    if (debugConsole) {
      const timestamp = new Date().toTimeString().slice(0,8);
      const entry = document.createElement('div');
      entry.style.fontSize = '10px';
      entry.innerHTML = `${timestamp} [${type.toUpperCase()}] ${message}`;
      debugConsole.appendChild(entry);
      debugConsole.scrollTop = debugConsole.scrollHeight;
    }
    console.log(`[${type}] ${message}`);
  }
  
  // Expose critical functions to global scope
  if (typeof window !== 'undefined') {
    window.draw = draw;
    window.drawFromSpec = drawFromSpec;
    window.DEFAULTS = DEFAULTS;
    window.lastData = lastData;
    window.initCanvas = initCanvas;
    window.load = load;
    window.applyValidationFix = applyValidationFix;
    window.debugLog = debugLog;
    
    console.log('Simulator functions exposed to window');
  }
  
  // Setup keyboard navigation
  function setupKeyboardNavigation() {
    document.addEventListener('keydown', (e) => {
      // Ignore if user is typing in an input field
      if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA' || e.target.tagName === 'SELECT') {
        return;
      }
      
      // Keyboard shortcuts
      switch(e.key.toLowerCase()) {
        case 'v':
          // Toggle validation panel
          const validationPanel = document.getElementById('validationPanel');
          if (validationPanel) {
            validationPanel.open = !validationPanel.open;
          }
          break;
          
        case 'd':
          // Toggle region inspector (debug panel)
          const regionInspector = document.getElementById('regionInspector');
          if (regionInspector) {
            regionInspector.open = !regionInspector.open;
          }
          break;
          
        case 'r':
          // Refresh display
          if (!e.ctrlKey && !e.metaKey) { // Don't interfere with browser refresh
            e.preventDefault();
            const refreshBtn = document.getElementById('refresh');
            if (refreshBtn) refreshBtn.click();
          }
          break;
        case 's':
          // Download screenshot
          if (!e.ctrlKey && !e.metaKey){
            const btn = document.getElementById('screenshot'); if (btn) btn.click();
          }
          break;
          
        case 'z':
          // Cycle zoom levels (1, 1.5, 2, 2.5, 3, 3.5, 4)
          if (!e.ctrlKey && !e.metaKey) { // Don't interfere with undo
            e.preventDefault();
            const zoomInput = document.getElementById('zoom');
            if (zoomInput) {
              let currentZoom = parseFloat(zoomInput.value);
              currentZoom += 0.5;
              if (currentZoom > 4) currentZoom = 1;
              zoomInput.value = currentZoom;
              zoomInput.dispatchEvent(new Event('input', { bubbles: true }));
            }
          }
          break;
          
        case 'g':
          // Toggle grid
          const gridCheckbox = document.getElementById('showGrid');
          if (gridCheckbox) {
            gridCheckbox.checked = !gridCheckbox.checked;
            gridCheckbox.dispatchEvent(new Event('change', { bubbles: true }));
          }
          break;
          
        case 'l':
          // Toggle labels
          const labelsCheckbox = document.getElementById('showLabels');
          if (labelsCheckbox) {
            labelsCheckbox.checked = !labelsCheckbox.checked;
            labelsCheckbox.dispatchEvent(new Event('change', { bubbles: true }));
          }
          break;
          
        case 'w':
          // Toggle partial refresh windows
          const windowsCheckbox = document.getElementById('showWindows');
          if (windowsCheckbox) {
            windowsCheckbox.checked = !windowsCheckbox.checked;
            windowsCheckbox.dispatchEvent(new Event('change', { bubbles: true }));
          }
          break;
          
        case '?':
          // Show keyboard shortcuts help
          if (e.shiftKey) {
            showKeyboardHelp();
          }
          break;
      }
      
      // Update zoom ARIA value when changed
      const zoomInput = document.getElementById('zoom');
      if (zoomInput && e.key.toLowerCase() === 'z') {
        zoomInput.setAttribute('aria-valuenow', zoomInput.value);
      }
    });
  }
  
  function showKeyboardHelp() {
    const helpText = `
Keyboard Shortcuts:
  V - Toggle validation panel
  D - Toggle debug/region inspector
  R - Refresh display
  Z - Cycle zoom levels
  G - Toggle grid overlay
  L - Toggle region labels
  W - Toggle partial refresh windows
  ? - Show this help (Shift+?)
    `.trim();
    
    alert(helpText);
  }

  // Persist open/closed state for inspector panels
  (function(){
    try{
      const suffix = (function(){
        try{
          const variant = (QS.get('variant') || (window.UI_SPEC && window.UI_SPEC.defaultVariant) || 'v1');
          const specMode = (typeof window !== 'undefined' && window.__specMode) ? String(window.__specMode) : 'v1';
          return `${variant}::${specMode}`;
        }catch(e){ return 'v1::v1'; }
      })();
      const key = (id)=>`sim_panel_open::${id}::${suffix}`;
      const wire = (id)=>{
        const el = document.getElementById(id);
        if (!el || typeof el.open === 'undefined') return;
        try{
          const saved = localStorage.getItem(key(id));
          if (saved !== null) el.open = saved === '1';
        }catch(e){}
        el.addEventListener('toggle', ()=>{
          try{ localStorage.setItem(key(id), el.open ? '1' : '0'); }catch(e){}
        });
      };
      wire('regionInspector');
      wire('validationPanel');
    }catch(e){}
  })();

  // Debug overlay system for visual debugging
  const DebugOverlay = {
    enabled: false,
    showBounds: true,
    showLabels: true,
    showTiming: false,
    highlightData: null,

    toggle() {
      this.enabled = !this.enabled;
      // Sync with UI checkbox if it exists
      const checkbox = document.getElementById('debugOverlayEnabled');
      if (checkbox) checkbox.checked = this.enabled;
      // Enable/disable dependent controls
      const boundsCheckbox = document.getElementById('debugOverlayBounds');
      const timingCheckbox = document.getElementById('debugOverlayTiming');
      const highlightSelect = document.getElementById('debugOverlayHighlight');
      const infoDiv = document.getElementById('debugOverlayInfo');
      if (boundsCheckbox) boundsCheckbox.disabled = !this.enabled;
      if (timingCheckbox) timingCheckbox.disabled = !this.enabled;
      if (highlightSelect) highlightSelect.disabled = !this.enabled;
      if (infoDiv) infoDiv.style.display = this.enabled ? 'block' : 'none';
      // Redraw
      if (window.draw && window.lastData) {
        window.draw(window.lastData);
      }
      console.log(`Debug overlay: ${this.enabled ? 'ON' : 'OFF'}`);
    },

    // Draw region boundaries with labels
    drawRegionBounds(ctx) {
      if (!this.enabled || !this.showBounds || !window.GJSON?.rects) return;

      ctx.save();
      ctx.strokeStyle = 'rgba(255, 0, 0, 0.5)';
      ctx.fillStyle = 'rgba(255, 0, 0, 0.7)';
      ctx.font = '8px monospace';
      ctx.lineWidth = 1;

      for (const [name, rect] of Object.entries(window.GJSON.rects)) {
        if (!rect || rect.length < 4) continue;
        const [x, y, w, h] = rect;

        // Draw rectangle
        ctx.strokeRect(x + 0.5, y + 0.5, w - 1, h - 1);

        // Draw label if enabled
        if (this.showLabels) {
          const shortName = name.replace(/^RECT_/, '').substring(0, 12);
          ctx.fillText(shortName, x + 2, y + 8);
        }
      }

      ctx.restore();
    },

    // Show render timing per region
    drawRenderTiming(ctx, timings) {
      if (!this.enabled || !this.showTiming || !timings) return;

      ctx.save();
      ctx.fillStyle = 'rgba(0, 0, 0, 0.7)';
      ctx.fillRect(0, 0, 150, Object.keys(timings).length * 10 + 10);

      ctx.fillStyle = '#0f0';
      ctx.font = '8px monospace';

      let y = 10;
      for (const [name, ms] of Object.entries(timings)) {
        const shortName = name.substring(0, 12);
        ctx.fillText(`${shortName}: ${ms.toFixed(1)}ms`, 2, y);
        y += 10;
      }

      ctx.restore();
    },

    // Highlight data flow for a specific field
    highlightDataFlow(ctx, fieldName) {
      if (!this.enabled || !this.highlightData) return;

      // Highlight regions based on field name (comprehensive mapping)
      const fieldRegionMap = {
        'room_name': ['RECT_HEADER_NAME'],
        'inside_temp': ['RECT_INSIDE_TEMP_F'],
        'outside_temp': ['RECT_OUTSIDE_TEMP_F'],
        'humidity': ['RECT_INSIDE_RH', 'RECT_OUTSIDE_RH'],
        'pressure': ['RECT_PRESS_HPA'],
        'weather': ['RECT_WEATHER_ICON', 'RECT_WEATHER'],
        'battery': ['RECT_BATT_LEVEL', 'RECT_BATT_VOLT'],
        'time': ['RECT_TIME']
      };

      const regions = fieldRegionMap[this.highlightData] || [];
      if (regions.length === 0) return;

      ctx.save();
      ctx.strokeStyle = '#0ff';
      ctx.lineWidth = 2;
      ctx.setLineDash([5, 5]);

      for (const regionName of regions) {
        const rect = window.GJSON?.rects?.[regionName];
        if (rect && rect.length >= 4) {
          ctx.strokeRect(rect[0], rect[1], rect[2], rect[3]);
        }
      }

      ctx.restore();
    }
  };

  // Keyboard shortcut: Ctrl+Shift+D toggles debug overlay
  document.addEventListener('keydown', (e) => {
    if (e.ctrlKey && e.shiftKey && e.key === 'D') {
      e.preventDefault();
      DebugOverlay.toggle();
    }
  });

  // Export for global access
  window.DebugOverlay = DebugOverlay;

  // Initialize render timings storage
  window.__renderTimings = {};

  // Theme toggle functionality
  function initTheme() {
    const savedTheme = localStorage.getItem('sim_theme');
    // Default to dark mode (no data-theme attribute needed)
    if (savedTheme === 'light') {
      document.documentElement.setAttribute('data-theme', 'light');
    }
    updateThemeButton();
  }

  function toggleTheme() {
    const isLight = document.documentElement.getAttribute('data-theme') === 'light';
    if (isLight) {
      document.documentElement.removeAttribute('data-theme');
      localStorage.setItem('sim_theme', 'dark');
    } else {
      document.documentElement.setAttribute('data-theme', 'light');
      localStorage.setItem('sim_theme', 'light');
    }
    updateThemeButton();
  }

  function updateThemeButton() {
    const btn = document.getElementById('themeToggle');
    if (!btn) return;
    const isLight = document.documentElement.getAttribute('data-theme') === 'light';
    btn.textContent = isLight ? '🌙' : '☀️';
    btn.title = isLight ? 'Switch to dark mode' : 'Switch to light mode';
  }

  // Initialize theme immediately (before DOM ready to prevent flash)
  initTheme();

  // Wait for DOM to be ready before initializing
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', () => {
      load();
      setupKeyboardNavigation();
      document.getElementById('themeToggle')?.addEventListener('click', toggleTheme);
    });
  } else {
    // DOM is already ready
    load();
    setupKeyboardNavigation();
    document.getElementById('themeToggle')?.addEventListener('click', toggleTheme);
  }
})();


