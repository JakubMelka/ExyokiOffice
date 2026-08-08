# Transitions

A transition is the effect PowerPoint plays when a slide appears. All 21
PresentationML transition effects are available, with typed options for the
parameters each effect supports.

The namespace aliases from [Presentations](presentations.md) are assumed.

## Setting a transition

```cpp
slide->SetTransition({.Kind = PresentationTransitionKind::Fade,
                      .Speed = PresentationTransitionSpeed::Medium,
                      .AdvanceOnClick = true});

slide->SetTransition({.Kind = PresentationTransitionKind::Push,
                      .Duration = 700,
                      .AdvanceAfter = 5000,
                      .Options = {.Direction = PresentationTransitionDirection::Down}});

slide->SetTransition({.Kind = PresentationTransitionKind::Split,
                      .Options = {.Direction = PresentationTransitionDirection::Out,
                                  .Orientation = PresentationTransitionOrientation::Vertical}});

slide->RemoveTransition();
```

Timing fields: `Speed` is the legacy three-step speed, `Duration` the exact
milliseconds (PowerPoint 2010+), `AdvanceOnClick` and `AdvanceAfter`
(milliseconds) control how the presentation moves on.

## Validation

Effect-specific parameters live in `PresentationTransitionOptions`; each
effect accepts only the parameters its own PresentationML type can express,
and `SetTransition` rejects (without touching the slide) any combination
PowerPoint could not represent — a corner direction on `Wipe`, an
orientation on `Circle`, and so on. The result reports the problem, and the
slide keeps its previous transition.

## Unsupported effects

An effect this API does not model is reported as
`PresentationTransitionKind::Unsupported` and its subtree is retained
verbatim while speed, duration, advance, and sound metadata remain
editable — opening and re-saving a deck never downgrades a transition the
API cannot express.
