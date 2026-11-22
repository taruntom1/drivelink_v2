const themeControl = document.querySelector(".theme-button");
const darkMode = matchMedia("(prefers-color-scheme: dark)");

const navToggle = document.querySelector(".nav-toggle");
const navigation = document.getElementById("guide-navigation");
const compactLayout = matchMedia("(max-width: 52rem)");

function syncNavigation() {
  navToggle.hidden = !compactLayout.matches;
  navigation.hidden = compactLayout.matches;
  navToggle.setAttribute("aria-expanded", String(!navigation.hidden));
}
syncNavigation();
compactLayout.addEventListener("change", syncNavigation);
navToggle.addEventListener("click", () => {
  navigation.hidden = !navigation.hidden;
  navToggle.setAttribute("aria-expanded", String(!navigation.hidden));
});

function updatePictures() {
  const dark = document.documentElement.dataset.theme === "dark" ||
    (!document.documentElement.dataset.theme && darkMode.matches);
  for (const source of document.querySelectorAll("source[data-dark]")) {
    source.media = dark ? "screen" : "not all";
  }
}
function applyTheme(theme) {
  if (theme === "system") {
    delete document.documentElement.dataset.theme;
  } else {
    document.documentElement.dataset.theme = theme;
  }
  themeControl.textContent = {system: "System", light: "Light", dark: "Dark"}[theme];
  themeControl.setAttribute("aria-label", "Color theme: " + theme + ". Change theme");
  updatePictures();
}

let theme = "system";
try {
  const saved = localStorage.getItem("drivelink-guide-theme");
  if (saved === "light" || saved === "dark") theme = saved;
} catch {
  // Storage can be disabled. Theme switching still works.
}
applyTheme(theme);
darkMode.addEventListener("change", updatePictures);
themeControl.hidden = false;
themeControl.addEventListener("click", () => {
  const next = {system: "light", light: "dark", dark: "system"};
  theme = next[theme];
  try {
    localStorage.setItem("drivelink-guide-theme", theme);
  } catch {
    // Keep the current page's choice.
  }
  applyTheme(theme);
});

for (const group of document.querySelectorAll("[data-choice-group]")) {
  const panels = [...group.querySelectorAll(":scope > [data-choice]")];
  const controls = document.createElement("div");
  controls.className = "choice-buttons";
  controls.setAttribute("role", "group");
  controls.setAttribute("aria-label", group.classList.contains("path-picker") ?
    "Choose your setup" : "Choose your platform");
  const buttons = panels.map((panel, index) => {
    const button = document.createElement("button");
    button.type = "button";
    button.textContent = panel.dataset.choice;
    button.addEventListener("click", () => select(index));
    controls.append(button);
    return button;
  });

  function select(index) {
    for (const [position, panel] of panels.entries()) {
      panel.hidden = position !== index;
      buttons[position].setAttribute("aria-pressed", String(position === index));
    }
  }

  group.prepend(controls);
  select(0);
}

if (navigator.clipboard) {
  for (const block of document.querySelectorAll("pre > code")) {
    const controls = document.createElement("div");
    controls.className = "code-controls";
    const button = document.createElement("button");
    button.type = "button";
    button.textContent = "Copy";
    button.setAttribute("aria-label", "Copy code");
    const status = document.createElement("span");
    status.setAttribute("role", "status");
    controls.append(button, status);
    block.parentElement.before(controls);
    button.addEventListener("click", async () => {
      status.textContent = "";
      try {
        await navigator.clipboard.writeText(block.textContent.trimEnd());
        status.textContent = "Copied";
      } catch {
        status.textContent = "Select the text to copy it.";
      }
    });
  }
}
