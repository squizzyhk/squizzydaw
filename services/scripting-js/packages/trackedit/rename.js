// Renames tracks from a pattern. Tokens: {n} running number, {nn} two-digit number, {name} the
// current name, {kind} Audio/Instrument/Bus/FX/Folder/Master.
//   args: selection (track ids; default every track), pattern (default "{kind} {n}"), start (default 1)
function formatName(pattern, track, number) {
  return pattern.replace(/\{(n|nn|name|kind)\}/g, (whole, token) => {
    switch (token) {
      case 'n': return String(number);
      case 'nn': return String(number).padStart(2, '0');
      case 'name': return track.name;
      default: return track.trackKindName;
    }
  });
}

function createRenameCommand() {
  return Host.Commands.handler({
    category: 'Track',
    name: 'Rename With Pattern',
    title: 'Rename Tracks',
    run(ctx) {
      const pattern = String(ctx.arg('pattern', '{kind} {n}'));
      let number = Math.trunc(Number(ctx.arg('start', 1)));
      const tracks = ctx.selection.tracks.length > 0 ? ctx.selection.tracks : ctx.doc.tracks;
      let changed = 0;
      for (const t of tracks) {
        const name = formatName(pattern, t, number++);
        if (name === t.name) continue;
        ctx.doc.execute(Host.Actions.SetProperty, { node: t.id, key: 'name', value: Host.Values.string(name) });
        ++changed;
      }
      return changed > 0;
    },
  });
}
