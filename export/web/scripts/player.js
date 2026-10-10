/* Transpiled from scripts/player.nxa — topdown WASD + coin pickup */
window.EYESCIRE_SCRIPTS = window.EYESCIRE_SCRIPTS || {};

window.EYESCIRE_SCRIPTS['player.nxa'] = {
  script_init(api) {
    api.eng_log('[player] ready');
  },
  script_update(api) {
    const me = api.eng_self();
    if (me < 0) return;
    const speed = 220.0;
    const dt = api.eng_dt_seconds();
    const step = speed * dt;
    let dx = 0, dy = 0;
    const VK = api.VK;
    if (api.key_down(VK.A) || api.key_down(VK.LEFT)) dx -= 1;
    if (api.key_down(VK.D) || api.key_down(VK.RIGHT)) dx += 1;
    if (api.key_down(VK.W) || api.key_down(VK.UP)) dy -= 1;
    if (api.key_down(VK.S) || api.key_down(VK.DOWN)) dy += 1;
    if (dx !== 0 || dy !== 0) {
      const len = Math.sqrt(dx * dx + dy * dy);
      const vx = (dx / len) * step | 0;
      const vy = (dy / len) * step | 0;
      api.ent_set_x(me, api.ent_x(me) + vx);
      api.ent_set_y(me, api.ent_y(me) + vy);
    }
    const coin = api.ent_first_overlap(me, 'coin');
    if (coin >= 0) {
      api.eng_log('[player] picked up ' + api.ent_name(coin));
      api.ent_destroy(coin);
    }
  }
};
