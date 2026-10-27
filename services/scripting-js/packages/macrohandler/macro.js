// Macros.
//
// Commands (category "Macro"):
//   Run        args: steps = [{ category, name, args? }, ...] (array or JSON text), title?
//              Each step is dispatched to whichever command handler claims it. The whole macro is
//              one transaction, so it is a single undo step; any failing step cancels everything.
//              A step without its own `selection` / `position` inherits the macro's.
//   Run Saved  args: name -> steps read from the setting Services.scripting-js.Macros.<name>
//
// Edit task (category "Action", for IEditor::execute):
//   Action Sequence  params: steps = [{ action: "{class id}", params: {...} }, ...] (array or JSON text)
//                    Runs the actions in order through the editor it receives.
const INHERITED = ['selection', 'position'];

function parseSteps(value, what) {
  let steps = value;
  if (typeof steps === 'string') {
    try {
      steps = JSON.parse(steps);
    } catch (e) {
      throw new Host.HostError(what + ': steps are not valid JSON (' + e.message + ')', Host.Results.INVALIDARG);
    }
  }
  if (!Array.isArray(steps)) throw new Host.HostError(what + ': steps must be an array', Host.Results.INVALIDARG);
  return steps;
}

function runSteps(ctx, steps) {
  for (let i = 0; i < steps.length; ++i) {
    const step = steps[i] || {};
    if (step.category === 'Macro') throw new Host.HostError('macros cannot call macros (step ' + (i + 1) + ')', Host.Results.INVALIDARG);
    const args = Object.assign({}, step.args || {});
    for (const key of INHERITED) if (args[key] === undefined && ctx.args[key] !== undefined) args[key] = ctx.args[key];
    try {
      Host.Commands.invoke(step.category, step.name, args);
    } catch (e) {
      throw new Host.HostError('macro step ' + (i + 1) + ' (' + step.category + '/' + step.name + '): ' + e.message, e.code || Host.Results.FAIL);
    }
  }
}

function createMacroCommands() {
  return Host.Commands.handler([
    {
      category: 'Macro', name: 'Run', title: 'Run Macro',
      enabled: (ctx) => ctx.args.steps !== undefined,
      run(ctx) {
        runSteps(ctx, parseSteps(ctx.args.steps, 'Run Macro'));
      },
    },
    {
      category: 'Macro', name: 'Run Saved', title: 'Run Saved Macro',
      enabled: (ctx) => typeof ctx.args.name === 'string' && Host.Settings.has('Services.scripting-js.Macros.' + ctx.args.name),
      run(ctx) {
        const text = Host.Settings.get('Services.scripting-js.Macros.' + ctx.args.name, '[]');
        runSteps(ctx, parseSteps(text, 'Run Saved Macro'));
      },
    },
  ]);
}

function createActionSequence() {
  return {
    interfaces: [Host.Interfaces.IAction],
    perform(doc, editor, params, result) {
      const steps = parseSteps(params.steps, 'Action Sequence');
      const nodes = [];
      for (const step of steps) {
        const out = editor.execute(step.action, step.params || {});
        if (out && out.node) nodes.push(out.node);
      }
      result.count = steps.length;
      if (nodes.length > 0) result.node = nodes[nodes.length - 1];
    },
  };
}
