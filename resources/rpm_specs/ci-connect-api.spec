Name: slate-api-server
Version: %{version}
Release: 1%{?dist}
Summary: API server for the CI-Connect
License: MIT
URL: https://github.com/maniaclab/ci-connect-api

Source0: ci-connect-api-%{version}.tar.gz

BuildRequires: gcc-c++ boost-devel zlib-devel openssl-devel libcurl-devel yaml-cpp-devel cmake3 aws-sdk-cpp-dynamodb-devel
Requires: boost zlib openssl libcurl yaml-cpp aws-sdk-cpp-dynamodb-libs

%description
CI-Connect API server

%prep
%setup -c -n ci-connect-api-%{version}.tar.gz 

%build
cd ci-connect-api
mkdir build
cd build
cmake3 -DCMAKE_INSTALL_PREFIX="$RPM_BUILD_ROOT/usr/" ..
make

%install
cd ci-connect-api
cd build
rm -rf "$RPM_BUILD_ROOT"
echo "$RPM_BUILD_ROOT"
make install
cd ..

%clean
rm -rf "$RPM_BUILD_ROOT"

%post
/sbin/ldconfig

%postun
/sbin/ldconfig

%files
%{_bindir}/ci-connect-service

%defattr(-,root,root,-)

