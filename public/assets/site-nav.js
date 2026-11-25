const productsMenu = document.querySelector(".site-menu");

if (productsMenu) {
  const summary = productsMenu.querySelector("summary");

  document.addEventListener("pointerdown", (event) => {
    if (!productsMenu.contains(event.target)) productsMenu.open = false;
  });

  productsMenu.addEventListener("focusout", (event) => {
    if (event.relatedTarget && !productsMenu.contains(event.relatedTarget)) {
      productsMenu.open = false;
    }
  });

  productsMenu.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && productsMenu.open) {
      event.preventDefault();
      productsMenu.open = false;
      summary.focus();
    }
  });

  window.addEventListener("pageshow", (event) => {
    if (event.persisted) productsMenu.open = false;
  });
}
