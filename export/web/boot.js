/* Loads script modules then starts the game */
(function () {
  function loadScript(src) {
    return new Promise(function (resolve, reject) {
      const s = document.createElement('script');
      s.src = src;
      s.onload = resolve;
      s.onerror = function () { console.warn('[web] script missing:', src); resolve(); };
      document.body.appendChild(s);
    });
  }

  function loadAll(list) {
    let chain = Promise.resolve();
    for (let i = 0; i < list.length; i++) {
      chain = chain.then(function () { return loadScript(list[i]); });
    }
    return chain;
  }

  const canvas = document.getElementById('game');
  const scene = window.EYESCIRE_SCENE || { entities: [] };
  const files = window.EYESCIRE_SCRIPT_FILES || ['scripts/player.js'];
  const title = scene.projectName || 'Game';
  document.title = title;

  loadAll(files).then(function () {
    EyescirePlayer.start({
      canvas: canvas,
      scene: scene,
      scripts: window.EYESCIRE_SCRIPTS || {},
      projectName: title
    });
  });
})();
