(() => {
  'use strict';
  const cache = new Map();
  const cacheBudget = 16 * 1024 * 1024;
  let cacheBytes = 0, active, showTimer, hideTimer, renderer;
  let popup, message, content, warnings;

  function node(tag, text, className) {
    const result = document.createElement(tag);
    if (text !== undefined) result.textContent = text;
    if (className) result.className = className;
    return result;
  }

  function dismiss() {
    clearTimeout(showTimer);
    clearTimeout(hideTimer);
    showTimer = hideTimer = undefined;
    if (active) {
      clearTimeout(active.timeout);
      active.render.abort();
      if (active.loading) active.load.abort();
      active.link.removeAttribute('aria-describedby');
    }
    active = undefined;
    if (popup) {
      popup.hidden = true;
      content.replaceChildren();
      warnings.replaceChildren();
    }
  }

  function laterDismiss() {
    clearTimeout(showTimer);
    clearTimeout(hideTimer);
    hideTimer = setTimeout(dismiss, 180);
  }

  function position() {
    if (!active || !active.link.isConnected) { dismiss(); return; }
    const anchor = active.link.getBoundingClientRect();
    const vw = document.documentElement.clientWidth, vh = window.innerHeight;
    if (anchor.bottom < 0 || anchor.top > vh) { dismiss(); return; }
    const width = Math.min(560, vw - 24);
    const right = anchor.right + 12, left = anchor.left - width - 12;
    const beside = right + width <= vw - 12 || left >= 12;
    const below = Math.max(0, vh - anchor.bottom - 24), above = Math.max(0, anchor.top - 24);
    const placeBelow = below >= Math.min(400, above);
    const height = beside ? vh - 24 : Math.max(80, placeBelow ? below : above);
    popup.style.width = width + 'px';
    popup.style.maxHeight = height + 'px';
    popup.dataset.availableHeight = String(height);
    popup.style.left = (right + width <= vw - 12 ? right : left >= 12 ? left :
      Math.max(12, Math.min(anchor.left, vw - width - 12))) + 'px';
    const box = popup.getBoundingClientRect();
    popup.style.top = Math.max(12, Math.min(vh - box.height - 12,
      beside ? anchor.top : placeBelow ? anchor.bottom + 12 : anchor.top - box.height - 12)) + 'px';
  }

  function createPopup() {
    if (popup) return;
    const style = node('style');
    style.textContent = `
      .pdf-preview{position:fixed;z-index:20;box-sizing:border-box;padding:12px;background:#fff;
        color:#17283d;border:1px solid #8293a5;border-radius:10px;box-shadow:0 10px 35px #142b4440;
        overflow:auto;overscroll-behavior:contain;font-size:.85rem}
      .pdf-preview[hidden]{display:none}.pdf-preview header{display:flex;align-items:start;gap:12px;
        justify-content:space-between;margin:0 0 10px;padding:0;position:sticky;top:0;background:#fff}
      .pdf-preview p{margin:0;overflow-wrap:anywhere}.pdf-preview button{font-size:.8rem;padding:5px 8px}
      .pdf-preview canvas{display:block;max-width:100%;height:auto;background:white;margin:auto}
      .pdf-preview .pdf-preview-warnings{margin-top:8px;color:#854200}
      .pdf-preview[data-error=true] .pdf-preview-status{color:#a01818}
      .pdf-preview-alert{position:fixed;bottom:12px;right:12px;max-width:400px;padding:12px;
        background:white;color:#a01818;border:1px solid #a01818;z-index:20}`;
    document.head.append(style);
    popup = node('aside', undefined, 'pdf-preview');
    popup.hidden = true;
    popup.setAttribute('role', 'region');
    popup.setAttribute('aria-label', 'Experimentelle PDF-Vorschau');
    const header = node('header'), close = node('button', 'Schlie\u00dfen');
    close.type = 'button';
    close.addEventListener('click', () => {
      const link = active?.link;
      dismiss();
      link?.focus({ preventScroll: true });
      clearTimeout(showTimer);
    });
    message = node('p', undefined, 'pdf-preview-status');
    message.id = 'pdf-preview-status';
    message.setAttribute('role', 'status');
    message.setAttribute('aria-live', 'polite');
    header.append(message, close);
    content = node('div');
    warnings = node('div', undefined, 'pdf-preview-warnings');
    popup.append(header, content, warnings);
    popup.addEventListener('pointerenter', () => clearTimeout(hideTimer));
    popup.addEventListener('pointerleave', event => {
      if (!active?.link.contains(event.relatedTarget)) laterDismiss();
    });
    document.body.append(popup);
  }

  function remember(hash, pdf, bytes) {
    if (bytes > cacheBudget) return;
    while (cache.size >= 2 || cacheBytes + bytes > cacheBudget) {
      const key = cache.keys().next().value;
      cacheBytes -= cache.get(key).bytes;
      cache.delete(key);
    }
    cache.set(hash, { pdf, bytes });
    cacheBytes += bytes;
  }

  async function show(link) {
    dismiss();
    createPopup();
    const request = {
      link, render: new AbortController(), load: new AbortController(), loading: false,
      deadline: performance.now() + 15000,
    };
    active = request;
    popup.hidden = false;
    popup.dataset.error = 'false';
    link.setAttribute('aria-describedby', message.id);
    content.replaceChildren();
    warnings.replaceChildren();
    message.textContent = 'Experimentelle PDF-Vorschau wird geladen \u2026';
    position();
    if (active !== request) return;
    request.timeout = setTimeout(() => {
      if (active !== request) return;
      request.render.abort();
      if (request.loading) request.load.abort();
      popup.dataset.error = 'true';
      message.textContent = 'Vorschau nach 15 Sekunden abgebrochen. Der Klick \u00f6ffnet weiterhin das RIS-Original.';
      content.replaceChildren();
      position();
    }, 15000);
    try {
      const hash = link.dataset.pdfHash, page = Number(link.dataset.pdfPage), bytes = Number(link.dataset.pdfBytes);
      if (!/^[a-f0-9]{64}$/.test(hash) || !Number.isSafeInteger(page) || page < 1 ||
          !Number.isSafeInteger(bytes) || bytes < 1) throw new Error('Ung\u00fcltige archivierte PDF-Fassung.');
      renderer ??= import('/pdf-crumb.js');
      const library = await renderer;
      if (active !== request || request.render.signal.aborted) return;
      if (bytes > library.DEFAULT_SECURITY_LIMITS.maxInputBytes)
        throw new Error('Diese PDF ist gr\u00f6\u00dfer als das sichere Vorschau-Limit von ' +
          Math.round(library.DEFAULT_SECURITY_LIMITS.maxInputBytes / 1048576) + ' MiB.');
      let pdf;
      if (cache.has(hash)) {
        const entry = cache.get(hash);
        cache.delete(hash); cache.set(hash, entry);
        pdf = entry.pdf;
      } else {
        request.loading = true;
        pdf = await library.loadPdfCrumb('/pdf/' + hash + '.pdf', { signal: request.load.signal });
        request.loading = false;
        if (active !== request || request.render.signal.aborted) return;
        remember(hash, pdf, bytes);
      }
      if (page > pdf.pages.length) throw new Error('Die indexierte PDF-Seite fehlt in dieser Fassung.');
      const selected = pdf.pages[page - 1];
      const box = selected.cropBox || selected.mediaBox;
      let width = Math.abs(box[2] - box[0]), height = Math.abs(box[3] - box[1]);
      const rotation = ((selected.rotate || 0) % 360 + 360) % 360;
      if (rotation === 90 || rotation === 270) [width, height] = [height, width];
      if (!Number.isFinite(width) || !Number.isFinite(height) || width <= 0 || height <= 0)
        throw new Error('Die PDF-Seitengr\u00f6\u00dfe ist ung\u00fcltig.');
      const canvas = node('canvas');
      canvas.hidden = true;
      canvas.setAttribute('role', 'img');
      canvas.setAttribute('aria-label', 'Vorschau der PDF-Seite ' + page);
      content.append(canvas);
      const scale = Math.min((popup.clientWidth - 26) / width,
        Math.max(60, Number(popup.dataset.availableHeight) - 140) / height, 1.5);
      const result = await pdf.renderPage(page - 1, canvas, { scale, signal: request.render.signal });
      if (active !== request || request.render.signal.aborted) return;
      if (performance.now() > request.deadline) throw new Error('Die Darstellung dauerte l\u00e4nger als 15 Sekunden.');
      canvas.style.height = 'auto';
      canvas.hidden = false;
      const notices = [...pdf.warnings];
      for (const [operator, count] of result.unsupportedOperators)
        notices.push('Nicht vollst\u00e4ndig unterst\u00fctzt: ' + operator + ' (' + count + ')');
      message.textContent = 'Experimentelle Vorschau \u00b7 Seite ' + page + ' / ' + pdf.pages.length +
        '. Klick auf den Link \u00f6ffnet das RIS-Original.';
      if (notices.length) {
        const details = node('details'), summary = node('summary',
          'Darstellung m\u00f6glicherweise unvollst\u00e4ndig \u00b7 ' + notices.length + ' Hinweise');
        const list = node('ul');
        for (const notice of notices) list.append(node('li', notice));
        details.append(summary, list);
        warnings.append(details);
      }
      position();
    } catch (error) {
      if (active !== request) return;
      if (request.render.signal.aborted) return;
      popup.dataset.error = 'true';
      content.replaceChildren();
      message.textContent = 'Keine Vorschau: ' + error.message + ' Der Klick \u00f6ffnet weiterhin das RIS-Original.';
      console.error('oparl-search: PDF preview failed', error);
      position();
    } finally {
      clearTimeout(request.timeout);
    }
  }

  function schedule(link) {
    clearTimeout(hideTimer);
    if (active?.link === link) return;
    dismiss();
    showTimer = setTimeout(() => {
      showTimer = undefined;
      if (link.isConnected) void show(link);
    }, 250);
  }

  async function initialize() {
    const response = await fetch('/api/preview-status', {
      signal: AbortSignal.timeout(8000), headers: { Accept: 'application/json' },
    });
    if (!response.ok) throw new Error('HTTP ' + response.status);
    const configuration = await response.json();
    if (configuration.enabled === false) return;
    if (configuration.enabled !== true) throw new Error('Ung\u00fcltige Vorschau-Konfiguration.');
    document.addEventListener('pointerover', event => {
      if (event.pointerType === 'touch') return;
      const link = event.target.closest?.('a[data-pdf-hash]');
      if (link && !link.contains(event.relatedTarget)) schedule(link);
    });
    document.addEventListener('pointerout', event => {
      const link = event.target.closest?.('a[data-pdf-hash]');
      if (link && !link.contains(event.relatedTarget) && !popup?.contains(event.relatedTarget)) laterDismiss();
    });
    document.addEventListener('focusin', event => {
      const link = event.target.closest?.('a[data-pdf-hash]');
      if (link) schedule(link);
      else if (popup?.contains(event.target)) clearTimeout(hideTimer);
    });
    document.addEventListener('focusout', event => {
      const link = event.target.closest?.('a[data-pdf-hash]');
      if ((link || popup?.contains(event.target)) && !popup?.contains(event.relatedTarget) &&
          !active?.link.contains(event.relatedTarget)) laterDismiss();
    });
    document.addEventListener('keydown', event => {
      if (event.key === 'Escape' && (active || showTimer)) { event.preventDefault(); dismiss(); }
    });
    window.addEventListener('blur', dismiss);
    window.addEventListener('scroll', () => { if (active) position(); }, { passive: true });
    window.addEventListener('resize', () => { if (active) position(); });
    new MutationObserver(() => {
      if (active && !active.link.isConnected) dismiss();
    }).observe(document.body, { childList: true, subtree: true });
  }

  void initialize().catch(error => {
    console.error('oparl-search: PDF preview initialization failed', error);
    const alert = node('p', 'PDF-Vorschau nicht verf\u00fcgbar: ' + error.message, 'pdf-preview-alert');
    alert.setAttribute('role', 'alert');
    document.body.append(alert);
  });
})();
