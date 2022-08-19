Name:   gst-vosk
Version:  0.2.0
Release:  1%{?dist}
Summary:  Gstreamer plugin for VOSK voice recognition engine

License:  GPLv3+
URL:    https://github.com/PhilippeRo/gst-vosk
Source0:  https://github.com/PhilippeRo/gst-vosk/archive/refs/tags/%{name}-%{version}.tar.gz

BuildRequires:  meson
BuildRequires:  gcc
BuildRequires:  gstreamer1-devel
BuildRequires:  glib2-devel
BuildRequires:  gettext

%description
Gstreamer plugin for VOSK voice recognition engine.

%prep
%autosetup

%build
%meson
%meson_build

%install
%meson_install
%find_lang %{name}

%files -f %{name}.lang
%license COPYING
%doc AUTHORS README.md
%{_libdir}/gstreamer-1.0/libgstvosk.so
%{_libdir}/libvosk.so

%changelog
* Sun Jul 31 2022 Philippe Rouquier <bonfire-app@wanadoo.fr> 0.1.0-1
- Initial version of the package
