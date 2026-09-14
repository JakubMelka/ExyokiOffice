# Animations

`AnimationEffects()`/`SetAnimationEffects()` model the slide's animation
sequences the way PowerPoint's animation pane does: an ordered list of
effects, each with a target shape, a gallery class (`Entrance`, `Emphasis`,
`Exit`, `MotionPath`), a concrete effect, a start relationship (`OnClick`,
`WithPrevious`, `AfterPrevious`), and timing.

The namespace aliases from [Presentations](presentations.md) are assumed.

## Adding effects

```cpp
slide->AddAnimationEffect({.TargetShapeId = title->Id(),
                           .Class = PresentationAnimationEffectClass::Entrance,
                           .Effect = PresentationAnimationEffect::Fly,
                           .Direction = PresentationAnimationDirection::Left});

slide->AddAnimationEffect({.TargetShapeId = body->Id(),
                           .Class = PresentationAnimationEffectClass::Emphasis,
                           .Effect = PresentationAnimationEffect::Spin,
                           .Trigger = PresentationAnimationTrigger::AfterPrevious,
                           .Timing = {.Delay = 250, .Duration = 1200, .AutoReverse = true},
                           .RotationDegrees = 360});
```

Effects target shapes by their stable ID (`shape->Id()`) — capture it before
any shape-tree reordering, which invalidates shape wrappers (see
[Shapes](shapes.md)).

Writing regenerates the whole `p:timing` tree — a timing root, a main
sequence, click groups, and the `presetClass`/`presetID`/`presetSubtype`
triples plus behaviors PowerPoint expects — so the result looks native in
PowerPoint's animation pane. The identifiers are PowerPoint's own gallery
values: `ChangeFillColor` is the Fill Color emphasis (`presetID="1"`,
`presetSubtype="2"`, with the `fill.type`/`fill.on` behaviors PowerPoint adds).
Emphasis preset 2 is PowerPoint's Change Font; a file written by an earlier
version of this library under that id is still read as `ChangeFillColor`
when its behaviors animate the fill colour, which no font effect does.

## Interactive triggers

```cpp
// Setting TriggerShapeId moves the effect into an interactive sequence that
// plays when that shape is clicked instead of on slide advance.
slide->AddAnimationEffect({.TargetShapeId = body->Id(),
                           .TriggerShapeId = button->Id(),
                           .Class = PresentationAnimationEffectClass::Exit,
                           .Effect = PresentationAnimationEffect::Fade});
```

The interactive sequence carries what PowerPoint's own "trigger" effect
writes and reads back as *On Click of* that shape: the `onClick` start
condition on the trigger shape, an `endSync` that ends the sequence with its
last effect, a zero delay on the click group, and a trailing `nextCondLst`
repeating the trigger.

## Reordering and removing

```cpp
auto effects = slide->AnimationEffects();
slide->MoveAnimationEffect(effects.front().Id, 2);
slide->RemoveAnimationEffect(effects.back().Id);
slide->ClearAnimationEffects();
```

Every effect is validated before anything is written, so one bad entry
leaves the slide untouched. Effects the API does not model round-trip as
`PresentationAnimationEffect::Unsupported` and their markup is preserved
across edits. Removing the last effect removes the `p:timing` element
altogether (unless media timing remains in it): an empty timing tree is one
PowerPoint refuses to open, and the next effect recreates the tree. A
`p:transition` on the slide is not affected.

## Low-level escape hatch

`AddAnimation`/`Animations` remain available for free-standing `p:anim`
behaviors below `p:timing/p:tnLst`, for animation constructs the effect
model does not cover.
