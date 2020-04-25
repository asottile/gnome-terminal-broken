gnome-terminal-broken
=====================

tab tearing can cause a crash in gnome-terminal so it was disabled upstream

I'd rather have periodic crashes and the feature  `¯\_(ツ)_/¯`

### build

```bash
./run
```

### install

```bash
sudo dpkg -i dist/*
```

### uninstall

```bash
sudo apt-get install --allow-downgrades \
    $(dpkg -l | grep asottile | awk '{print $2"="$3}' | sed 's/+asottile1//')
```

### updating

```bash
dget --allow-unauthenticated --download-only whatever.dsc
gbp import-dsc *.dsc \
    --debian-branch ubuntu/... \
    --upstream-branch upstream \
    --create-missing-branches
gbp pq import
babi src/terminal-notebook.c  # redo the patch
git commit -am 're-enable-tab-tearing'
gbp pq export
cp debian/patches/re-enable-tab-tearing.patch .
```
