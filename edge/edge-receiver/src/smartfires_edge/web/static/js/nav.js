const NAV_LINKS = [
  { href: "/", label: "Main" },
  { href: "/map-history", label: "Map & History" },
];

function renderNav(activePath) {
  const nav = document.createElement("nav");
  nav.className = "topnav";
  for (const link of NAV_LINKS) {
    const a = document.createElement("a");
    a.href = link.href;
    a.textContent = link.label;
    if (link.href === activePath) {
      a.classList.add("active");
    }
    nav.appendChild(a);
  }
  document.body.prepend(nav);
}
