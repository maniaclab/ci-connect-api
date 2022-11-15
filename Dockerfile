FROM centos:7

RUN yum update -y 
RUN yum install -y \
	gcc-c++ \
	openssl-devel \
	libcurl-devel \
	zlib-devel \
	boost-devel \
	make \
	epel-release
RUN yum install -y cmake3 

RUN yum localinstall -y \
	https://jenkins.slateci.io/artifacts/static/aws-sdk-cpp-core-libs-1.7.25-1.el7.x86_64.rpm \
	https://jenkins.slateci.io/artifacts/static/aws-sdk-cpp-core-devel-1.7.25-1.el7.x86_64.rpm \
	https://jenkins.slateci.io/artifacts/static/aws-sdk-cpp-dynamodb-devel-1.7.25-1.el7.x86_64.rpm \
	https://jenkins.slateci.io/artifacts/static/aws-sdk-cpp-dynamodb-libs-1.7.25-1.el7.x86_64.rpm \
	https://jenkins.slateci.io/artifacts/static/aws-sdk-cpp-route53-devel-1.7.25-1.el7.x86_64.rpm \
	https://jenkins.slateci.io/artifacts/static/aws-sdk-cpp-route53-libs-1.7.25-1.el7.x86_64.rpm

RUN mkdir /api
COPY include /api/include
COPY src /api/src
COPY cmake /api/cmake
COPY CMakeLists.txt /api/CMakeLists.txt

RUN cd /api; mkdir build; cd build; cmake3 ..
RUN cd /api/build; make -j

CMD /api/build/ci-connect-service
