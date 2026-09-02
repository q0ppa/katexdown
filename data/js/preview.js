// Render glue for the Katexdown plugin.
// Exposes a small API the C++ side drives via runJavaScript():
//   __setMarkdown(text)      render markdown source
//   __applyVars(obj)         set CSS custom properties on <html>
//   __useHljsTheme(name)     enable one bundled hljs <style>, disable the rest
//   __setCodeCss(css)        inject a generated hljs theme (Application mode)
//   __setColorScheme(dark)   set color-scheme + data attribute on <html>
//   __setImageMode(mode)     image decode policy: 'eager' | 'auto' | 'saver'
//                            (see the image-mode section below)
//   __serializedHtml()       outerHTML with the image machinery normalized
//                            away (real srcs, no lazy/placeholder state) —
//                            what export serializes

(function () {
  "use strict";

  function escapeHtml(s) {
    return s
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;");
  }

  var md = window.markdownit({
    html: true,
    linkify: true,
    typographer: false,
    breaks: false,
    highlight: function (str, lang) {
      var hljs = window.hljs;
      var body;
      if (hljs && lang && hljs.getLanguage(lang)) {
        try {
          body = hljs.highlight(str, { language: lang, ignoreIllegals: true }).value;
        } catch (e) {
          body = escapeHtml(str);
        }
      } else if (hljs) {
        try {
          body = hljs.highlightAuto(str).value;
        } catch (e) {
          body = escapeHtml(str);
        }
      } else {
        body = escapeHtml(str);
      }
      return '<pre class="hljs"><code>' + body + "</code></pre>";
    },
  });

  md.use(taskLists);
  md.use(githubAlerts);

  // Image decode policy (driven from the C++ settings): the eager mode leaves
  // markdown-it's output alone so every image decodes at once (the classic
  // behavior); auto/saver give every image the native lazy-loading +
  // async-decoding attributes, and the image manager further below parks the
  // far off-screen ones on a placeholder so Chromium never decodes them.
  var kdxDefaultImage = md.renderer.rules.image;
  md.renderer.rules.image = function (tokens, idx, options, env, self) {
    if (imgMode !== "eager") {
      tokens[idx].attrSet("loading", "lazy");
      tokens[idx].attrSet("decoding", "async");
    }
    return kdxDefaultImage(tokens, idx, options, env, self);
  };

  // Math (LaTeX): texmath.min.js defines a top-level `texmath` function when
  // inlined as a classic script. Both assets come from the data dir (see
  // tools/fetch-assets.py); without them `$` stays literal and nothing breaks.
  if (window.texmath && window.katex) {
    try {
      window.markdownitTeXMath = window.markdownitTeXMath || window.texmath;
      md.use(window.markdownitTeXMath, {
        engine: window.katex,
        delimiters: "dollars",
        katexOptions: { throwOnError: false },
      });
    } catch (e) {
      // leave math disabled rather than breaking the whole preview
    }
  }

  // GitHub alerts: > [!NOTE] / [!TIP] / [!IMPORTANT] / [!WARNING] / [!CAUTION]
  function githubAlerts(md) {
    var ICONS = {
      note: '<svg class="octicon" viewBox="0 0 16 16" width="16" height="16" aria-hidden="true" style="fill:currentColor;margin-right:8px"><path d="M0 8a8 8 0 1 1 16 0A8 8 0 0 1 0 8Zm8-6.5a6.5 6.5 0 1 0 0 13 6.5 6.5 0 0 0 0-13ZM6.5 7.75A.75.75 0 0 1 7.25 7h1a.75.75 0 0 1 .75.75v2.75h.25a.75.75 0 0 1 0 1.5h-2a.75.75 0 0 1 0-1.5h.25v-2h-.25a.75.75 0 0 1-.75-.75ZM8 6a1 1 0 1 1 0-2 1 1 0 0 1 0 2Z"/></svg>',
      tip: '<svg class="octicon" viewBox="0 0 16 16" width="16" height="16" aria-hidden="true" style="fill:currentColor;margin-right:8px"><path d="M8 1.5c-2.363 0-4 1.69-4 3.75 0 .984.424 1.625.984 2.304l.214.253c.223.264.47.556.673.848.284.411.537.896.621 1.49a.75.75 0 0 1-1.484.211c-.04-.282-.163-.547-.37-.847a8.456 8.456 0 0 0-.542-.68c-.084-.1-.173-.205-.268-.32C3.201 7.75 2.5 6.766 2.5 5.25 2.5 2.31 4.863 0 8 0s5.5 2.31 5.5 5.25c0 1.516-.701 2.5-1.328 3.259-.095.115-.184.22-.268.319-.207.245-.383.453-.541.681-.208.3-.33.565-.37.847a.751.751 0 0 1-1.485-.212c.084-.593.337-1.078.621-1.489.203-.292.45-.584.673-.848.075-.088.147-.173.213-.253.561-.679.985-1.32.985-2.304 0-2.06-1.637-3.75-4-3.75ZM5.75 12h4.5a.75.75 0 0 1 0 1.5h-4.5a.75.75 0 0 1 0-1.5ZM6 15.25a.75.75 0 0 1 .75-.75h2.5a.75.75 0 0 1 0 1.5h-2.5a.75.75 0 0 1-.75-.75Z"/></svg>',
      important: '<svg class="octicon" viewBox="0 0 16 16" width="16" height="16" aria-hidden="true" style="fill:currentColor;margin-right:8px"><path d="M0 1.75C0 .784.784 0 1.75 0h12.5C15.216 0 16 .784 16 1.75v9.5A1.75 1.75 0 0 1 14.25 13H8.06l-2.573 2.573A1.458 1.458 0 0 1 3 14.543V13H1.75A1.75 1.75 0 0 1 0 11.25Zm1.75-.25a.25.25 0 0 0-.25.25v9.5c0 .138.112.25.25.25h2a.75.75 0 0 1 .75.75v2.19l2.72-2.72a.749.749 0 0 1 .53-.22h6.5a.25.25 0 0 0 .25-.25v-9.5a.25.25 0 0 0-.25-.25Zm7 2.25v2.5a.75.75 0 0 1-1.5 0v-2.5a.75.75 0 0 1 1.5 0ZM9 9a1 1 0 1 1-2 0 1 1 0 0 1 2 0Z"/></svg>',
      warning: '<svg class="octicon" viewBox="0 0 16 16" width="16" height="16" aria-hidden="true" style="fill:currentColor;margin-right:8px"><path d="M6.457 1.047c.659-1.234 2.427-1.234 3.086 0l6.082 11.378A1.75 1.75 0 0 1 14.082 15H1.918a1.75 1.75 0 0 1-1.543-2.575Zm1.763.707a.25.25 0 0 0-.44 0L1.698 13.132a.25.25 0 0 0 .22.368h12.164a.25.25 0 0 0 .22-.368Zm.53 3.996v2.5a.75.75 0 0 1-1.5 0v-2.5a.75.75 0 0 1 1.5 0ZM9 11a1 1 0 1 1-2 0 1 1 0 0 1 2 0Z"/></svg>',
      caution: '<svg class="octicon" viewBox="0 0 16 16" width="16" height="16" aria-hidden="true" style="fill:currentColor;margin-right:8px"><path d="M4.47.22A.749.749 0 0 1 5 0h6c.199 0 .389.079.53.22l4.25 4.25c.141.14.22.331.22.53v6a.749.749 0 0 1-.22.53l-4.25 4.25A.749.749 0 0 1 11 16H5a.749.749 0 0 1-.53-.22L.22 11.53A.749.749 0 0 1 0 11V5c0-.199.079-.389.22-.53Zm.84 1.28L1.5 5.31v5.38l3.81 3.81h5.38l3.81-3.81V5.31L10.69 1.5ZM8 4a.75.75 0 0 1 .75.75v3.5a.75.75 0 0 1-1.5 0v-3.5A.75.75 0 0 1 8 4Zm0 8a1 1 0 1 1 0-2 1 1 0 0 1 0 2Z"/></svg>',
    };
    var LABELS = { note: "Note", tip: "Tip", important: "Important", warning: "Warning", caution: "Caution" };
    var RE = /^\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\]\s*$/i;

    md.core.ruler.after("inline", "github-alerts", function (state) {
      var tokens = state.tokens;
      for (var i = 0; i < tokens.length; i++) {
        if (tokens[i].type !== "blockquote_open") {
          continue;
        }
        var para = tokens[i + 1];
        var inline = tokens[i + 2];
        if (!para || para.type !== "paragraph_open" || !inline || inline.type !== "inline") {
          continue;
        }
        var first = inline.children && inline.children[0];
        if (!first || first.type !== "text") {
          continue;
        }
        var m = RE.exec(first.content);
        if (!m) {
          continue;
        }
        var type = m[1].toLowerCase();

        tokens[i].tag = "div";
        tokens[i].attrSet("class", "markdown-alert markdown-alert-" + type);
        var depth = 0;
        for (var j = i; j < tokens.length; j++) {
          if (tokens[j].type === "blockquote_open") {
            depth++;
          } else if (tokens[j].type === "blockquote_close") {
            depth--;
            if (depth === 0) {
              tokens[j].tag = "div";
              break;
            }
          }
        }

        inline.children.shift(); // drop the [!TYPE] marker
        if (inline.children[0] && inline.children[0].type === "softbreak") {
          inline.children.shift();
        }

        var title = new state.Token("html_block", "", 0);
        title.block = true;
        title.content = '<p class="markdown-alert-title">' + ICONS[type] + LABELS[type] + "</p>";
        tokens.splice(i + 1, 0, title);
      }
    });
  }

  // GitHub-flavored task list checkboxes (adapted from markdown-it-task-lists).
  function taskLists(md) {
    md.core.ruler.after("inline", "github-task-lists", function (state) {
      var tokens = state.tokens;
      for (var i = 2; i < tokens.length; i++) {
        if (isTodoItem(tokens, i)) {
          todoify(tokens[i], state.Token);
          attrSet(tokens[i - 2], "class", "task-list-item");
          var p = parentList(tokens, i - 2);
          if (p >= 0) attrSet(tokens[p], "class", "contains-task-list");
        }
      }
    });
    function attrSet(token, name, value) {
      var idx = token.attrIndex(name);
      if (idx < 0) token.attrPush([name, value]);
      else token.attrs[idx][1] = value;
    }
    function parentList(tokens, index) {
      var target = tokens[index].level - 1;
      for (var i = index - 1; i >= 0; i--) {
        if (tokens[i].level === target) return i;
      }
      return -1;
    }
    function isTodoItem(tokens, i) {
      return (
        tokens[i].type === "inline" &&
        tokens[i - 1].type === "paragraph_open" &&
        tokens[i - 2].type === "list_item_open" &&
        /^\[[ xX]\] /.test(tokens[i].content)
      );
    }
    function todoify(token, Token) {
      var checked = /^\[[xX]\] /.test(token.content);
      var box = new Token("html_inline", "", 0);
      box.content =
        '<input class="task-list-item-checkbox"' +
        (checked ? ' checked=""' : "") +
        ' disabled="" type="checkbox"> ';
      token.children.unshift(box);
      token.children[1].content = token.children[1].content.replace(/^\[[ xX]\] /, "");
    }
  }

  var current = "";

  // Leading YAML frontmatter (very first line "---" through a closing "---")
  // renders as a GitHub-style metadata table instead of an <hr> plus raw text.
  var FRONT_MATTER = /^---[ \t]*\r?\n([\s\S]*?\n)?---[ \t]*(?:\r?\n|$)/;

  function frontMatterTable(src) {
    var m = FRONT_MATTER.exec(src);
    if (!m) {
      return null;
    }
    var data;
    try {
      data = window.jsyaml ? window.jsyaml.load(m[1] || "") : null;
    } catch (e) {
      return null;
    }
    if (!data || typeof data !== "object" || Array.isArray(data)) {
      return null;
    }
    var rows = "";
    Object.keys(data).forEach(function (key) {
      var v = data[key];
      var text = v == null ? "" : typeof v === "object" ? JSON.stringify(v) : String(v);
      rows += "<tr><td>" + escapeHtml(key) + "</td><td>" + escapeHtml(text) + "</td></tr>";
    });
    if (!rows) {
      return null;
    }
    return { html: "<table>" + rows + "</table>\n", body: src.slice(m[0].length) };
  }

  function rerender() {
    var el = document.getElementById("content");
    if (!el) {
      return;
    }
    var fm = frontMatterTable(current);
    el.innerHTML = (fm ? fm.html : "") + md.render(fm ? fm.body : current);
    rebuildOutline();
    armImages();
  }

  window.__setMarkdown = function (text) {
    current = text;
    rerender();
  };

  window.__applyVars = function (vars) {
    var root = document.documentElement.style;
    for (var k in vars) {
      if (Object.prototype.hasOwnProperty.call(vars, k)) {
        root.setProperty(k, vars[k]);
      }
    }
  };

  window.__useHljsTheme = function (name) {
    var ids = ["hljs-gh", "hljs-ghd"];
    var keep = name === "github-dark" ? "hljs-ghd" : name === "github" ? "hljs-gh" : null;
    ids.forEach(function (id) {
      var e = document.getElementById(id);
      if (e) e.disabled = id !== keep;
    });
    var gen = document.getElementById("hljs-generated");
    if (gen) gen.textContent = ""; // bundled theme active, clear generated
  };

  window.__setCodeCss = function (css) {
    var ids = ["hljs-gh", "hljs-ghd"];
    ids.forEach(function (id) {
      var e = document.getElementById(id);
      if (e) e.disabled = true; // generated theme takes over
    });
    var gen = document.getElementById("hljs-generated");
    if (!gen) {
      gen = document.createElement("style");
      gen.id = "hljs-generated";
      document.head.appendChild(gen);
    }
    gen.textContent = css;
  };

  window.__setColorScheme = function (dark) {
    document.documentElement.setAttribute("data-pv-scheme", dark ? "dark" : "light");
    document.documentElement.style.setProperty("color-scheme", dark ? "dark" : "light");
  };

  // ---------------------------------------------------------------------
  // Image decode policy ("image memory mode"), mirroring Settings::ImageMode:
  //   "eager"  every image decodes as soon as it is in the document, no matter
  //            where it sits — the classic, heaviest behavior.
  //   "auto"   images carry loading="lazy" (decode as they approach the
  //            viewport). Once a document passes IMG_AUTO_ARM_AT images the
  //            far off-screen ones are additionally parked on a transparent
  //            placeholder; decoded renderer memory then stays proportional to
  //            the region around the viewport instead of the whole document.
  //   "saver"  the same machinery as auto, but always on and with a tighter
  //            keep zone (only images near the viewport ever decode, and each
  //            is released the moment it scrolls out of the zone).
  // Parking is a pure swap of the displayed resource and must never move the
  // layout: an image's real src is stashed in data-kdx and replaced with a
  // 1x1 transparent GIF, so Chromium has nothing left to decode — and the
  // decoded bitmap of an image that scrolled away is released. swapOut() only
  // parks an image whose layout box is known — it has decoded once (its
  // width/height/aspect-ratio were recorded, see recordDims) or the author
  // pinned both dimensions — so the box (and the document height) survives
  // the swap untouched. A lazy image that never decoded has no decoded memory
  // to free and no known box, so it is left alone until the browser loads it
  // near the viewport; its box appears then, exactly like on any
  // lazy-loading page, and from its first decode on it is parked the same
  // way. An IntersectionObserver whose root margin is the keep zone swaps the
  // real src back in as an image approaches the viewport and parks it again
  // once it has left. base.css adds height:auto for the modes that manage
  // images, so the recorded width/height/aspect-ratio scale like the real
  // image did (e.g. under max-width:100%).
  var imgMode = "auto";
  // Auto arms the parking machinery once a document passes this many images.
  var IMG_AUTO_ARM_AT = 12;
  // Keep-zone size as a multiple of the viewport height (per mode): images
  // within viewport +/- zone keep (or regain) their real src.
  var IMG_ZONE_FACTOR = { auto: 2.5, saver: 1.3 };
  var IMG_PLACEHOLDER =
    "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";
  var imgObserver = null;
  var imgRearmTimer = null;

  function imgZonePx() {
    var f = imgMode === "saver" ? IMG_ZONE_FACTOR.saver : IMG_ZONE_FACTOR.auto;
    return Math.max(400, Math.round(window.innerHeight * f));
  }

  // Remember the image's box (from its first decode) so swapping the src to
  // the 1x1 placeholder does not collapse the layout. Author-pinned dimensions
  // are left alone (kdxSized "0"); otherwise the natural size is stored as
  // width/height attributes plus an explicit aspect ratio. The recording only
  // ever happens from a real decode (parked images ignore their load events),
  // so a parked image always has a known box when it is parked.
  function recordDims(img) {
    if (img.dataset.kdxSized || img.naturalWidth <= 0 || img.naturalHeight <= 0) {
      return;
    }
    // Never mistake the 1x1 parking placeholder for the real image. Its load
    // event can arrive after swapIn() already restored the real src (the
    // placeholder finished decoding while the real fetch was still pending),
    // so "is parked" is not a reliable discriminator there — the decoded
    // 1x1 size is. A genuinely 1x1 image needs no box recorded anyway: its
    // box is 1x1 with or without width/height attributes.
    if (img.naturalWidth === 1 && img.naturalHeight === 1) {
      return;
    }
    if (img.getAttribute("width") === null && img.getAttribute("height") === null) {
      img.setAttribute("width", String(img.naturalWidth));
      img.setAttribute("height", String(img.naturalHeight));
      img.style.aspectRatio = img.naturalWidth + " / " + img.naturalHeight;
      img.dataset.kdxSized = "1";
    } else {
      img.dataset.kdxSized = "0";
    }
  }

  // Park an image: stash the real src and drop the 1x1 placeholder in
  // (freeing the decoded bitmap once it had one). Swapping the src must never
  // change the layout, so this only parks an image whose box is known: it has
  // decoded (naturalWidth > 0 — recordDims below fixes its size) or the
  // author pinned both width and height attributes. An image that never
  // decoded has no decoded memory to free and no known box — parking it
  // would collapse the document height below it, so it keeps its real src
  // and stays native-lazy instead.
  function swapOut(img) {
    var src = img.getAttribute("src");
    if (img.dataset.kdx || img.dataset.kdxSkip || img.dataset.kdxFailed || !src || src === IMG_PLACEHOLDER) {
      return;
    }
    if (img.naturalWidth <= 0 && !(img.hasAttribute("width") && img.hasAttribute("height"))) {
      return; // box unknown: parking would shift the document height
    }
    recordDims(img);
    img.dataset.kdx = src;
    img.removeAttribute("srcset");
    img.setAttribute("src", IMG_PLACEHOLDER);
  }

  // Bring an image back: restore the real src (the placeholder disappears; the
  // recorded box keeps the layout stable while it decodes again).
  function swapIn(img) {
    var real = img.dataset.kdx;
    if (!real) {
      return;
    }
    delete img.dataset.kdx;
    img.removeAttribute("src");
    img.setAttribute("src", real);
  }

  function imgInZone(img) {
    var r = img.getBoundingClientRect();
    var z = imgZonePx();
    return r.bottom > -z && r.top < window.innerHeight + z;
  }

  function onImgLoad(e) {
    var img = e.target;
    // A load while the image is parked can only be the placeholder (the real
    // src is stashed in data-kdx until swapIn): there is nothing to record,
    // and the placeholder's 1x1 decode must never be mistaken for the image.
    if (img.dataset.kdx) {
      return;
    }
    recordDims(img);
    // A decode may finish after the image already scrolled out of the zone
    // (fast scrolling): release it right away instead of waiting for the
    // observer's next crossing.
    if (!imgInZone(img)) {
      swapOut(img);
    }
  }

  function onImgError(e) {
    // Leave failed images alone: no decode memory to save, and a broken-image
    // indicator is more honest than an empty placeholder. While the image is
    // parked, an error can only be an artifact of parking (the placeholder
    // never fails; an in-flight fetch of the real src may be aborted by the
    // swap) — not a verdict on the real src, which swapIn may still fetch
    // successfully from cache. Marking it failed here would strand the image
    // on the placeholder forever.
    if (e.target.dataset.kdx) {
      return;
    }
    e.target.dataset.kdxFailed = "1";
  }

  function handleImgIntersect(entries) {
    for (var i = 0; i < entries.length; i++) {
      var entry = entries[i];
      var img = entry.target;
      if (img.dataset.kdxSkip || img.dataset.kdxFailed) {
        continue;
      }
      if (entry.isIntersecting) {
        // Re-entered the keep zone: decode again.
        if (img.dataset.kdx) {
          swapIn(img);
        }
      } else if (!img.dataset.kdx) {
        // Left the keep zone: release the decoded bitmap.
        swapOut(img);
      }
    }
  }

  // (Re)apply the current mode to the rendered document. Called at the end of
  // every re-render, so it works on freshly built elements; also re-runs on
  // resize (the keep zone scales with the viewport).
  function armImages() {
    var content = document.getElementById("content");
    if (imgObserver) {
      imgObserver.disconnect();
      imgObserver = null;
    }
    if (!content) {
      return;
    }
    document.documentElement.setAttribute("data-pv-imgmode", imgMode);
    if (imgMode === "eager") {
      return; // eager mode never touches images
    }
    var imgs = content.querySelectorAll("img");
    var manage = imgMode === "saver" || imgs.length > IMG_AUTO_ARM_AT;
    var i;
    for (i = 0; i < imgs.length; i++) {
      var img = imgs[i];
      if (!img.hasAttribute("loading")) {
        img.setAttribute("loading", "lazy");
      }
      if (!img.hasAttribute("decoding")) {
        img.setAttribute("decoding", "async");
      }
      if (img.dataset.kdxSkip) {
        continue;
      }
      if (img.hasAttribute("srcset")) {
        img.dataset.kdxSkip = "1"; // srcset stays native-lazy only
        continue;
      }
      if (!manage) {
        continue;
      }
      if (!img.dataset.kdxListen) {
        img.dataset.kdxListen = "1";
        img.addEventListener("load", onImgLoad);
        img.addEventListener("error", onImgError);
      }
      if (img.dataset.kdxFailed || img.dataset.kdx || !img.getAttribute("src")) {
        continue;
      }
      recordDims(img);
      if (!imgInZone(img)) {
        // Far off-screen: park it — swapOut refuses unless the box is known,
        // so an image that never decoded stays native-lazy instead of
        // collapsing the document height.
        swapOut(img);
      }
    }
    if (!manage) {
      return;
    }
    var zone = imgZonePx() + "px 0px " + imgZonePx() + "px 0px";
    imgObserver = new IntersectionObserver(handleImgIntersect, { rootMargin: zone });
    for (i = 0; i < imgs.length; i++) {
      var el = imgs[i];
      if (!el.dataset.kdxSkip && !el.dataset.kdxFailed) {
        imgObserver.observe(el);
      }
    }
  }

  function scheduleImgRearm() {
    if (imgMode === "eager" || imgRearmTimer) {
      return;
    }
    imgRearmTimer = setTimeout(function () {
      imgRearmTimer = null;
      armImages();
    }, 150);
  }
  window.addEventListener("resize", scheduleImgRearm);

  window.__setImageMode = function (mode) {
    var clean = mode === "eager" || mode === "saver" ? mode : "auto";
    document.documentElement.setAttribute("data-pv-imgmode", clean);
    if (imgMode === clean) {
      return;
    }
    imgMode = clean;
    rerender(); // re-applies the renderer rule and re-arms the images
  };

  // Export hook: the standalone HTML must always carry plain, eager images
  // (real srcs, no lazy/placeholder state, no recorded sizes), regardless of
  // the live page's image mode. Work on a detached clone so the live page is
  // untouched (nothing re-fetches or re-decodes).
  window.__serializedHtml = function () {
    var root = document.documentElement.cloneNode(true);
    var imgs = root.querySelectorAll("img");
    for (var i = 0; i < imgs.length; i++) {
      var img = imgs[i];
      if (img.dataset.kdx) {
        img.setAttribute("src", img.dataset.kdx);
      }
      if (img.dataset.kdxSrcset) {
        img.setAttribute("srcset", img.dataset.kdxSrcset);
      }
      if (img.dataset.kdxSized === "1") {
        img.removeAttribute("width");
        img.removeAttribute("height");
        img.style.removeProperty("aspect-ratio");
      }
      img.removeAttribute("loading");
      img.removeAttribute("decoding");
      img.removeAttribute("data-kdx");
      img.removeAttribute("data-kdx-srcset");
      img.removeAttribute("data-kdx-skip");
      img.removeAttribute("data-kdx-sized");
      img.removeAttribute("data-kdx-failed");
      img.removeAttribute("data-kdx-listen");
    }
    root.removeAttribute("data-pv-imgmode");
    return root.outerHTML;
  };

  // ---------------------------------------------------------------------
  // Floating section outline: a round button pinned to the bottom-right of
  // the preview opens the list of headings (levels chosen in the config
  // page, H1-H5 by default); clicking an entry scrolls to that section.
  // ---------------------------------------------------------------------
  var DEFAULT_OUTLINE_LEVELS = [1, 2, 3, 4, 5];
  var outlineLevels = DEFAULT_OUTLINE_LEVELS.slice();
  var outlineBtn = null;
  var outlinePanel = null;
  var outlineList = null;
  var outlineTakenIds = {};

  function ensureOutlineUi() {
    if (outlineBtn) {
      return;
    }
    var btn = document.createElement("button");
    btn.type = "button";
    btn.id = "kdx-outline-btn";
    btn.title = "Jump to section";
    btn.setAttribute("aria-label", "Jump to section");
    btn.setAttribute("aria-expanded", "false");
    btn.style.display = "none"; // only shown when the document has headings
    btn.innerHTML =
      '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M2 4a1 1 0 1 1 0-2 1 1 0 0 1 0 2Zm0 5a1 1 0 1 1 0-2 1 1 0 0 1 0 2Zm0 5a1 1 0 1 1 0-2 1 1 0 0 1 0 2ZM6 4.75A.75.75 0 0 1 6.75 4h8.5a.75.75 0 0 1 0 1.5h-8.5A.75.75 0 0 1 6 4.75Zm0 5a.75.75 0 0 1 .75-.75h8.5a.75.75 0 0 1 0 1.5h-8.5A.75.75 0 0 1 6 9.75Zm.75 4.25a.75.75 0 0 0 0 1.5h8.5a.75.75 0 0 0 0-1.5Z"/></svg>';

    var panel = document.createElement("div");
    panel.id = "kdx-outline-panel";
    panel.hidden = true;
    var list = document.createElement("ul");
    list.id = "kdx-outline-list";
    panel.appendChild(list);

    var root = document.body || document.documentElement;
    root.appendChild(btn);
    root.appendChild(panel);

    btn.addEventListener("click", function () {
      setOutlineOpen(panel.hidden);
    });
    // A click anywhere outside the control closes the panel again.
    document.addEventListener("click", function (e) {
      if (panel.hidden) {
        return;
      }
      if (panel.contains(e.target) || btn.contains(e.target)) {
        return;
      }
      setOutlineOpen(false);
    });

    outlineBtn = btn;
    outlinePanel = panel;
    outlineList = list;
  }

  function outlineHeadingText(el) {
    return (el.innerText || el.textContent || "").replace(/\s+/g, " ").trim();
  }

  // GitHub-style slug: lowercase, punctuation collapses to dashes. Unicode
  // letters (e.g. Chinese headings) are kept.
  function outlineSlug(text) {
    return (
      text
        .toLowerCase()
        .replace(/[^\p{L}\p{N}]+/gu, "-")
        .replace(/^-+|-+$/g, "") || "section"
    );
  }

  function outlineUniqueId(text) {
    var base = outlineSlug(text);
    if (!outlineTakenIds[base] && !document.getElementById(base)) {
      outlineTakenIds[base] = true;
      return base;
    }
    var n = 2;
    while (outlineTakenIds[base + "-" + n] || document.getElementById(base + "-" + n)) {
      n += 1;
    }
    outlineTakenIds[base + "-" + n] = true;
    return base + "-" + n;
  }

  function setOutlineOpen(open) {
    if (!outlinePanel) {
      return;
    }
    outlinePanel.hidden = !open;
    outlineBtn.setAttribute("aria-expanded", open ? "true" : "false");
  }

  function jumpToSection(el) {
    el.scrollIntoView({ behavior: "smooth", block: "start" });
    // Restart the flash animation on the heading we just landed on.
    el.classList.remove("kdx-outline-jumped");
    void el.offsetWidth;
    el.classList.add("kdx-outline-jumped");
    setOutlineOpen(false);
  }

  function rebuildOutline() {
    ensureOutlineUi();
    outlineTakenIds = {};
    var content = document.getElementById("content");
    var items = [];
    if (content) {
      var headings = content.querySelectorAll("h1, h2, h3, h4, h5, h6");
      for (var i = 0; i < headings.length; i++) {
        var el = headings[i];
        var level = parseInt(el.tagName.charAt(1), 10);
        if (outlineLevels.indexOf(level) < 0) {
          continue;
        }
        var text = outlineHeadingText(el);
        if (!text) {
          continue;
        }
        // Anchor id (kept when the heading already has one, e.g. raw HTML)
        // so in-page "#slug" links resolve like on GitHub.
        if (!el.id) {
          el.id = outlineUniqueId(text);
        }
        items.push({ el: el, level: level, text: text });
      }
    }

    outlineList.textContent = "";
    outlineBtn.style.display = items.length ? "" : "none";
    outlinePanel.hidden = true;
    if (!items.length) {
      return;
    }

    var minLevel = items[0].level;
    for (var j = 1; j < items.length; j++) {
      if (items[j].level < minLevel) {
        minLevel = items[j].level;
      }
    }
    for (var k = 0; k < items.length; k++) {
      (function (item) {
        var li = document.createElement("li");
        li.className = "kdx-outline-item";
        li.setAttribute("role", "button");
        li.setAttribute("tabindex", "0");
        li.setAttribute("data-level", String(item.level));
        li.style.paddingLeft = 8 + (item.level - minLevel) * 14 + "px";
        li.title = item.text;

        var lvl = document.createElement("span");
        lvl.className = "kdx-outline-lvl";
        lvl.textContent = "H" + item.level;
        var text = document.createElement("span");
        text.className = "kdx-outline-text";
        text.textContent = item.text;
        li.appendChild(lvl);
        li.appendChild(text);

        function activate() {
          jumpToSection(item.el);
        }
        li.addEventListener("click", activate);
        li.addEventListener("keydown", function (e) {
          if (e.key === "Enter" || e.key === " ") {
            e.preventDefault();
            activate();
          }
        });
        outlineList.appendChild(li);
      })(items[k]);
    }
  }

  // Which heading levels the outline lists (driven from the plugin settings).
  window.__setOutlineLevels = function (levels) {
    var clean = [];
    for (var i = 0; levels && i < levels.length; i++) {
      var lvl = Math.floor(Number(levels[i]));
      if (lvl >= 1 && lvl <= 6 && clean.indexOf(lvl) < 0) {
        clean.push(lvl);
      }
    }
    clean.sort(function (a, b) {
      return a - b;
    });
    outlineLevels = clean;
    rebuildOutline();
  };

  // preview.js lives in <head>, so the document body does not exist yet at
  // parse time; build the control once the DOM is ready. This also makes a
  // standalone exported .html re-wire the control from the already-rendered
  // headings.
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", function () {
      ensureOutlineUi();
      rebuildOutline();
    });
  } else {
    ensureOutlineUi();
    rebuildOutline();
  }
})();
