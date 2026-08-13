(function() {
  "use strict";

  var kryonIde = "https://kryonlabs.com/ide.html";
  var rawBase = "https://raw.githubusercontent.com/kryonlabs/kryon/master/examples/";
  var examples = [
    ["Theme", "03_theme.kry"],
    ["Layout", "07_layout.kry"],
    ["Basic Controls", "11_basic_controls.kry"],
    ["Collections", "12_collections.kry"],
    ["Canvas", "14_canvas.kry"],
    ["Containers", "15_containers.kry"],
    ["Text Editor", "13_text_editor.kry"],
    ["Pictures", "19_pictures.kry"],
    ["Animation", "23_animation.kry"],
    ["Tilemap", "24_tilemap.kry"]
  ];
  var frame = document.querySelector("[data-session-frame]");
  var input = document.querySelector("[data-source-url]");
  var form = document.querySelector("[data-source-form]");
  var list = document.querySelector("[data-examples]");

  function sessionUrl(src) {
    return kryonIde + "?src=" + encodeURIComponent(src);
  }

  function openSource(src, push) {
    input.value = src;
    frame.src = sessionUrl(src);
    if (push) {
      var url = new URL(window.location.href);
      url.searchParams.set("src", src);
      window.history.replaceState(null, "", url.toString());
    }
  }

  function renderExamples() {
    examples.forEach(function(item) {
      var button = document.createElement("button");
      var src = rawBase + item[1];
      button.type = "button";
      button.className = "example-session";
      button.innerHTML = "<span>" + item[0] + "</span><code>" + item[1] + "</code>";
      button.addEventListener("click", function() {
        openSource(src, true);
      });
      list.appendChild(button);
    });
  }

  form.addEventListener("submit", function(ev) {
    ev.preventDefault();
    if (input.value.trim()) openSource(input.value.trim(), true);
  });

  renderExamples();
  openSource(new URLSearchParams(window.location.search).get("src") || rawBase + examples[0][1], false);
})();
