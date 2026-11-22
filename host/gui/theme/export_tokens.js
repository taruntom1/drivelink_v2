// Read-only Figma export of the DriveLink design tokens.
// Run through the Figma MCP `use_figma` tool against the design file
// 49NRHfKe8rOoVlloLLCqLh (see host/gui/README.md, "Theme"). It changes nothing
// in the file. Save the returned JSON as host/gui/theme/tokens.json, then run
// python host/gui/theme/gen_theme.py.

const round = (x) => Math.round(x * 10000) / 10000;
const value = (v) => {
  if (v && typeof v === 'object' && v.type === 'VARIABLE_ALIAS') return { alias: v.id };
  if (v && typeof v === 'object' && 'r' in v) {
    return { r: round(v.r), g: round(v.g), b: round(v.b), a: round('a' in v ? v.a : 1) };
  }
  return v;
};

const collections = await figma.variables.getLocalVariableCollectionsAsync();
const variables = await figma.variables.getLocalVariablesAsync();
const names = Object.fromEntries(variables.map((v) => [v.id, v.name]));
const byCollection = {};
for (const v of variables) (byCollection[v.variableCollectionId] ??= []).push(v);

const out = { file: '49NRHfKe8rOoVlloLLCqLh', collections: [], textStyles: [], effectStyles: [] };
for (const c of collections) {
  const modes = Object.fromEntries(c.modes.map((m) => [m.modeId, m.name]));
  out.collections.push({
    name: c.name,
    modes: c.modes.map((m) => m.name),
    variables: (byCollection[c.id] ?? [])
      .sort((a, b) => a.name.localeCompare(b.name))
      .map((v) => {
        const values = {};
        for (const [modeId, raw] of Object.entries(v.valuesByMode)) {
          const val = value(raw);
          values[modes[modeId]] = val && val.alias ? { alias: names[val.alias] } : val;
        }
        return { name: v.name, type: v.resolvedType, description: v.description, values };
      }),
  });
}

const bound = (style) => Object.fromEntries(
  Object.entries(style.boundVariables ?? {}).map(([k, a]) => [k, names[a.id]]));
for (const s of await figma.getLocalTextStylesAsync()) {
  out.textStyles.push({
    name: s.name, description: s.description, family: s.fontName.family,
    style: s.fontName.style, size: s.fontSize, lineHeight: s.lineHeight,
    letterSpacing: s.letterSpacing, bound: bound(s),
  });
}
for (const s of await figma.getLocalEffectStylesAsync()) {
  out.effectStyles.push({
    name: s.name, description: s.description,
    effects: s.effects.map((e) => ({
      type: e.type, radius: e.radius, spread: e.spread ?? 0,
      offset: e.offset ?? { x: 0, y: 0 }, color: e.color ? value(e.color) : null,
    })),
  });
}
return out;
