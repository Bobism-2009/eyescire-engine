(() => {
  const nav = document.querySelector("[data-nav]");
  const toggle = document.querySelector("[data-nav-toggle]");
  const links = document.querySelector("[data-nav-links]");
  const heroImg = document.querySelector("[data-hero-img]");
  const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  const onScroll = () => {
    if (!nav) return;
    nav.classList.toggle("is-scrolled", window.scrollY > 8);
    if (heroImg && !reduceMotion) {
      const y = Math.min(window.scrollY, 420);
      heroImg.style.transform = `scale(${1.06 - y * 0.00005}) translate3d(0, ${y * 0.12}px, 0)`;
    }
  };

  window.addEventListener("scroll", onScroll, { passive: true });
  onScroll();

  if (toggle && links) {
    toggle.addEventListener("click", () => {
      const open = links.classList.toggle("is-open");
      toggle.setAttribute("aria-expanded", open ? "true" : "false");
    });
    links.querySelectorAll("a").forEach((a) => {
      a.addEventListener("click", () => {
        links.classList.remove("is-open");
        toggle.setAttribute("aria-expanded", "false");
      });
    });
  }

  const picker = document.querySelector("[data-platform-picker]");
  if (picker) {
    const tabs = [...picker.querySelectorAll("[data-platform]")];
    const panels = [...picker.querySelectorAll("[data-platform-panel]")];
    const show = (id) => {
      tabs.forEach((tab) => {
        const on = tab.getAttribute("data-platform") === id;
        tab.classList.toggle("is-active", on);
        tab.setAttribute("aria-selected", on ? "true" : "false");
      });
      panels.forEach((panel) => {
        const on = panel.getAttribute("data-platform-panel") === id;
        panel.classList.toggle("is-active", on);
        panel.hidden = !on;
      });
    };
    tabs.forEach((tab) => {
      tab.addEventListener("click", () => show(tab.getAttribute("data-platform")));
    });
  }

  const tocLinks = [...document.querySelectorAll(".docs-toc a")];
  if (tocLinks.length) {
    const sections = tocLinks
      .map((a) => document.querySelector(a.getAttribute("href")))
      .filter(Boolean);
    const setActive = () => {
      let current = sections[0];
      const y = window.scrollY + 120;
      sections.forEach((sec) => {
        if (sec.offsetTop <= y) current = sec;
      });
      tocLinks.forEach((a) => {
        a.classList.toggle("is-active", a.getAttribute("href") === `#${current.id}`);
      });
    };
    window.addEventListener("scroll", setActive, { passive: true });
    setActive();
  }

  const nodes = document.querySelectorAll("[data-reveal]");
  if (reduceMotion || !("IntersectionObserver" in window)) {
    nodes.forEach((n) => n.classList.add("is-in"));
    return;
  }

  const io = new IntersectionObserver(
    (entries) => {
      entries.forEach((entry) => {
        if (entry.isIntersecting) {
          entry.target.classList.add("is-in");
          io.unobserve(entry.target);
        }
      });
    },
    { rootMargin: "0px 0px -8% 0px", threshold: 0.12 }
  );

  nodes.forEach((n) => io.observe(n));
})();
