/* Krait web bootstrap: Module config + loading screen for the canvas build. */
(function () {
  'use strict';

  var statusElement = document.querySelector('#status');
  var progressElement = document.querySelector('#progress');
  var loadingScreen = document.querySelector('#loading-screen');

  function hideLoading() {
    if (loadingScreen) loadingScreen.classList.add('is-hidden');
  }

  window.Module = {
    canvas: document.getElementById('canvas'),
    print: function (text) { console.log(text); },
    printErr: function (text) { console.warn(text); },
    setStatus: function (text) {
      if (statusElement) statusElement.textContent = text;
      var m = text && text.match(/(\d+)\/(\d+)/);
      if (progressElement) {
        if (m) {
          progressElement.value = parseInt(m[1], 10);
          progressElement.max = parseInt(m[2], 10);
          progressElement.hidden = false;
        } else if (!text) {
          hideLoading();
        }
      }
    },
    onRuntimeInitialized: function () {},
    preRun: [],
    postRun: [hideLoading]
  };

  window.addEventListener('error', function (e) {
    if (statusElement) statusElement.textContent = 'Error: ' + e.message;
  });
})();
