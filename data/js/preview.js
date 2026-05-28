// Render glue for the Katdown plugin.
// Exposes a small API the C++ side drives via runJavaScript():
//   __setMarkdown(text)      render markdown source
//   __applyVars(obj)         set CSS custom properties on <html>
//   __useHljsTheme(name)     enable one bundled hljs <style>, disable the rest
//   __setCodeCss(css)        inject a generated hljs theme (Application mode)
//   __setColorScheme(dark)   set color-scheme + data attribute on <html>

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
})();
