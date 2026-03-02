# Repository hooks

These hooks enforce/warn on BTstack version alignment.

## Install once per clone

```bash
./scripts/setup_hooks.sh
```

This sets:

```bash
git config core.hooksPath .githooks
```

## Behavior

- `post-checkout` (branch checkouts): warn if BTstack trees are not aligned
- `post-merge`: warn if BTstack trees are not aligned
- `pre-push`: warn by default if BTstack trees are not aligned
  - optional strict mode per push:
    ```bash
    BTSTACK_ALIGNMENT_ENFORCE_PRE_PUSH=1 git push
    ```

Check command used by hooks:

```bash
./patches/check_btstack_alignment.sh
```

## Temporary bypass

Not recommended, but available for emergencies:

```bash
SKIP_BTSTACK_ALIGNMENT_CHECK=1 git push
```
