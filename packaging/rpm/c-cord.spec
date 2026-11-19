%define _build_id_links none

Name:           c-cord
Version:        %{ccord_version}
Release:        1%{?dist}
Summary:        Fast, lightweight native C23 Discord client
License:        AGPL-3.0-only
URL:            https://c-cord.org

%description
C-Cord is a fast, feather-light native desktop Discord client written in C23
with built-in themes, hardware-accelerated streaming, and sandboxed Lua plugins.

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
cp -a %{ccord_stage}/. %{buildroot}/

%post
/usr/bin/update-desktop-database /usr/share/applications >/dev/null 2>&1 || :
/usr/bin/gtk-update-icon-cache -q /usr/share/icons/hicolor >/dev/null 2>&1 || :

%postun
/usr/bin/update-desktop-database /usr/share/applications >/dev/null 2>&1 || :
/usr/bin/gtk-update-icon-cache -q /usr/share/icons/hicolor >/dev/null 2>&1 || :

%files
/usr/bin/ccord
/usr/bin/c-cord
/usr/bin/ccord-plugin-host
/usr/share/ccord
/usr/share/applications/C-Cord.desktop
/usr/share/icons/hicolor/*/apps/C-Cord.png
