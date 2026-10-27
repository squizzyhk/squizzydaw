// Removes audio and instrument tracks that have no clips.  args: selection (limit to these tracks)
const KEEP_KINDS = new Set([3, 4, 5, 6]); // bus, fx, folder, master are never "empty"

function emptyTracks(ctx) {
  const pool = ctx.selection.tracks.length > 0 ? ctx.selection.tracks : ctx.doc.tracks;
  return pool.filter((t) => !KEEP_KINDS.has(t.trackKind) && t.clipCount === 0);
}

function createCleanupCommand() {
  return Host.Commands.handler({
    category: 'Track',
    name: 'Remove Empty',
    title: 'Remove Empty Tracks',
    enabled: (ctx) => emptyTracks(ctx).length > 0,
    run(ctx) {
      for (const t of emptyTracks(ctx).reverse()) ctx.doc.execute(Host.Actions.RemoveNode, { node: t.id });
    },
  });
}
