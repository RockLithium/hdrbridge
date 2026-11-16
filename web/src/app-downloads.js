export function initializeAppDownloads() {
  const container = document.querySelector("#app-download");
  const toggle = document.querySelector("#app-download-toggle");
  const menu = document.querySelector("#app-download-menu");
  if (!container || !toggle || !menu) return;

  let closeTimer;
  let pointerReleaseTimer;
  let pointerInside = false;
  let pointerDownInside = false;
  let openedByHover = false;
  const close = () => {
    clearTimeout(closeTimer);
    openedByHover = false;
    menu.hidden = true;
    toggle.setAttribute("aria-expanded", "false");
  };
  const open = () => {
    clearTimeout(closeTimer);
    menu.hidden = false;
    toggle.setAttribute("aria-expanded", "true");
  };

  container.addEventListener("pointerenter", (event) => {
    pointerInside = true;
    if (event.pointerType === "mouse") { openedByHover = menu.hidden; open(); }
  });
  container.addEventListener("pointerleave", (event) => {
    pointerInside = false;
    if (event.pointerType === "mouse" && !container.contains(document.activeElement)) {
      closeTimer = setTimeout(() => {
        if (!pointerDownInside && !container.contains(document.activeElement)) close();
      }, 180);
    }
  });
  toggle.addEventListener("click", () => {
    if (openedByHover) { openedByHover = false; open(); }
    else if (menu.hidden) open();
    else close();
  });
  toggle.addEventListener("keydown", (event) => {
    if (event.key === "ArrowDown") {
      event.preventDefault(); open(); menu.querySelector("a").focus();
    }
  });
  menu.addEventListener("click", (event) => {
    if (!event.target.closest?.("a[href]")) return;
    clearTimeout(closeTimer);
    // Let the link's native download/navigation start before hiding it.
    closeTimer = setTimeout(close, 0);
  });
  container.addEventListener("focusout", (event) => {
    if (container.contains(event.relatedTarget)) return;
    queueMicrotask(() => {
      if (!pointerDownInside && !container.contains(document.activeElement)) close();
    });
  });
  document.addEventListener("pointerdown", (event) => {
    clearTimeout(pointerReleaseTimer);
    pointerDownInside = container.contains(event.target);
    if (pointerDownInside) clearTimeout(closeTimer);
    else close();
  });
  const releasePointer = (event) => {
    clearTimeout(pointerReleaseTimer);
    // Keep the menu mounted through the link's click and native navigation.
    pointerReleaseTimer = setTimeout(() => {
      pointerDownInside = false;
      if (event.pointerType === "mouse" && !pointerInside && !container.contains(document.activeElement)) close();
    }, 0);
  };
  document.addEventListener("pointerup", releasePointer);
  document.addEventListener("pointercancel", releasePointer);
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && !menu.hidden) {
      const focused = container.contains(document.activeElement);
      close();
      if (focused) toggle.focus();
    }
  });
}
