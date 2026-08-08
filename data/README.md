# NOTICE

These files are auto-generated and files in this directory should not be changed without the appropriate changes in the back end processor.

## Files

- `namespaces.json`: List of known namespaces and their prefixes
- `schematrons.json`: Schematron constrain information
- `parts`: Directory contains information about each part
- `schemas`: Directory contains information about each schema element separated by namespace
- `typed`: Directory contains information used for generating strongly typed classes separated by namespace

## ExyokiOffice additions

Three files here belong to this repository rather than the import, and are meant
to survive a re-import of everything above:

- `exyokioffice_part_paths.json`: per-family folders for the parts Office
  keeps in one fixed place. An imported part descriptor names one folder
  relative to whatever parent the part is attached to, which cannot express that
  an image belongs in `/word/media` whether it hangs off the main document or a
  header, or that a slide master's theme belongs in `/ppt/theme` rather than
  `/ppt/slideMasters/theme`. The overlay supplies absolute folders per document
  family; the generator applies it after loading `parts` and fails the build when
  it names a part that no longer exists.
- `exyokioffice_ambiguous_elements.json`: the class the element factory
  creates for an element name that several classes declare (`w:rPr` is a
  paragraph mark's run properties and a style's run properties, `x:sheetData` is
  a worksheet's cell table and an Office2010 extension). The factory resolves a
  name without knowing the parent, so one class has to be nominated; parsing
  that walks the parent's content model resolves the name through the particle
  tree instead and is unaffected. Deriving the winner from the element order
  would leave it to a sort whose tie order differs between standard library
  implementations, which made the same package parse into different classes on
  Windows and Linux. The generator fails the build when the file names an
  element that is no longer ambiguous or a class that does not declare it, and
  warns about an ambiguous name the file does not cover.
- `exyokioffice_particle_extras.json`: content-model additions for types the
  import models more narrowly than the applications write them.
  `w:CT_RPrBaseStyleable` carries no run-property extensions, yet Word puts
  `w14:ligatures` into the document defaults of every file it saves. An entry
  names an existing type and a list of particle items to append to its
  top-level content model; the appended items get no `Children` entry, so they
  widen only what the validator accepts and add nothing to the generated API.
  The generator fails the build when the file names a type that no longer
  exists or that has no content model to extend. Add an entry only for markup a
  released Office application actually writes, and record which one in
  `Reason`.