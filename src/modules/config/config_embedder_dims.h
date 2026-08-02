/* config_embedder_dims.h — THE embedding width declaration.
 *
 * One number, one place. Everything in the tree reads it through config's API
 * (config_embedder_dims_default / _effective / _current, config_database.h); this
 * header exists only so the handful of leaf translation units that must not link
 * the config module — the db2 test shim — can still see the single declaration
 * instead of keeping a copy that drifts.
 *
 * Do not add logic here, and do not read this macro from anywhere that can call
 * config: the accessors apply the operator's pin and the EMBEDDER_DIMS
 * override, and bypassing them reintroduces exactly the split this replaced (a kb
 * that sized its columns 1024 wide while the bundled model returned 384).
 *
 * 384 is what the bundled bekko-a25m returns. Selecting a different embedder —
 * including an external one via the wizard's dimension field — writes that width
 * into config, which then wins over this default.
 */
#ifndef DEC_CONFIG_EMBEDDING_DIM_H
#define DEC_CONFIG_EMBEDDING_DIM_H 1

#define CONFIG_EMBEDDER_DIMS_DEFAULT 384

#endif /* DEC_CONFIG_EMBEDDING_DIM_H */
