// Mutes the selected clips, or unmutes them all when every one is already muted.
//   args: selection
const CLIP_MUTED = 0x01;

function createMuteCommand() {
  return Host.Commands.handler({
    category: 'Edit',
    name: 'Toggle Clip Mute',
    title: 'Mute/Unmute Clips',
    enabled: (ctx) => ctx.selectedClips().length > 0,
    run(ctx) {
      const clips = ctx.selectedClips();
      const mute = !clips.every((c) => (c.clipInfo.flags & CLIP_MUTED) !== 0);
      for (const clip of clips) {
        const muted = (clip.clipInfo.flags & CLIP_MUTED) !== 0;
        // engine/doc property names (INode::getProperties): "muted" is a bool.
        if (muted !== mute) ctx.doc.execute(Host.Actions.SetProperty, { node: clip.id, key: 'muted', value: mute });
      }
    },
  });
}
