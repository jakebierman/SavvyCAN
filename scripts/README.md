# Scripts

To run this scripts, first you must authorize your terminal to run:

```bash
chmod +x script/*.sh
```

Then you can run by using this command:

```bash
./scripts/the/script/you/want/to/run
```

## Releases

`release.sh` validates and publishes an annotated release tag. Run it without
`--push` first for a dry run:

```bash
./scripts/release.sh v222
./scripts/release.sh v222 --push
```

See `help/releasing.md` for the complete release workflow.
