// Render glue for the Kate Markdown Preview plugin.
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

  function rerender() {
    var el = document.getElementById("content");
    if (el) el.innerHTML = md.render(current);
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
