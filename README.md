gnome-terminal-broken
=====================

tab tearing can cause a crash in gnome-terminal so it was disabled upstream

I'd rather have periodic crashes and the feature  ¯\_(ツ)_/¯

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

