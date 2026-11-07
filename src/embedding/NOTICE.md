# Native embeddings

These are project-owned inference and indexing sources, initially developed
in a temporary EmbeddingGemma experiment. Initial import checksums are recorded
in `source.json`; this is not a separately maintained vendor dependency.
Its private JSON symbols are namespaced to coexist with OParl's parser.
The shared pinned newos runtime contains the upstream worker-claim fix;
no second runtime or local runtime patch is used.
Section indexing/loading accepts explicitly reported zero-content years.

The model `google/embeddinggemma-2`, revision recorded in `model.json`,
is published under Apache License 2.0. The architecture/reference operations
follow Google's published configuration and the Apache-2.0 Transformers
EmbeddingGemma 2 implementation. Model files are not included here.

The user-owned application source is maintained within this project.
Upstream newos is CC0. Native model artefacts must retain their own provenance
and licence information when distributed.
