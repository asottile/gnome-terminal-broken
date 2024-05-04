gnome-terminal-broken
=====================

tab tearing can cause a crash in gnome-terminal so it was disabled upstream

I'd rather have periodic crashes and the feature  `¯\_(ツ)_/¯`

this is available as a [PPA]

```bash
add-apt-repository ppa:asottile/gnome-terminal-tab-tearing
```

[PPA]: https://launchpad.net/~asottile/+archive/ubuntu/gnome-terminal-tab-tearing

### instructions for me

```bash
# substitute $DIST and $DSC below
gbp import-dsc \
    --allow-unauthenticated \
    --create-missing-branches \
    --debian-branch=ubuntu/"$DIST" \
    --pristine-tar \
    --upstream-branch=upstream \
    "$DSC"

# left as an exercise: `gpb pq import` and cherry-pick patch, etc.

# left as an exercise: in docker run the following:

gbp buildpackage \
    --git-builder="debuild -S -sa" \
    --git-debian-branch=ubuntu/"$DIST" \
    --git-pristine-tar
```
