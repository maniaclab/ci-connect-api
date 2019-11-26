# Building

## Dependencies

- [CMake](https://cmake.org)
- [OpenSSL](https://www.openssl.org)
- [libcurl](https://curl.haxx.se/libcurl/)
- [zlib](https://www.zlib.net)
- [Boost](https://www.boost.org)
- [Amazon AWS C++ SDK](https://github.com/aws/aws-sdk-cpp)

The majority of the dependencies can be installed on a fresh CentOS 7 system as follows. Note that the CentOS 7 Cmake package is too old to build the AWS SDK, so it is necessary to use the `cmake3` package from EPEL. This also means that all `cmake` commands must be replaced with `cmake3` on CentOS systems. 

	sudo yum install -y gcc-c++
	sudo yum install -y openssl-devel
	sudo yum install -y libcurl-devel
	sudo yum install -y zlib-devel
	sudo yum install -y boost-devel
	sudo yum install -y epel-release
	sudo yum install -y cmake3

Official RPMs do not seem to be available for the AWS SDK so it must be either built directly, or [unofficial RPMs](https://jenkins.slateci.io/artifacts/static/) can be used. Only the 'core' and 'dynamodb' components of the AWS SDK are needed by this project. 

## Compiling

In a copy of the source code:

	mkdir build
	cd build
	cmake .. [options] # use cmake3 on CentOS 7
	make

# Operating

## Options

Every option which can be set as an argument to the executable can also be set via an environment variable. For an option `--option` the corresponding variable name is `CICONNECT_option`. Note that the case of the option name does not change. Environment variables are read first, then options specified as arguments, so the latter take precedence. 

- --awsAccessKey The access key to use for DynamoDB
- --awsSecretKey The secret key to use for DynamoDB
- --awsRegion The AWS region to use for DynamoDB. Default: us-east-1
- --awsURLScheme The URL scheme to use for DynamoDB; may be either `http` or `https`. Default: http
- --awsEndpoint The hostname and port portion of the URL to use for DynamoDB. Default: localhost:8000
- --port The port number on which to listen. Default: 18080
- --sslCertificate A TLS certificate to use when accepting requests. If not specified only plain HTTP requests will be accepted, but this is not recommended. 
- --sslKey The secret key data for the TLS certificate. Must be specified if `--sslCertificate` is specified. 
- --bootstrapUserFile The path to the file which sets the root API user properties (see below). Default: base_connect_user
- --mailgunEndpoint The hostname and port portion of the URL to use for MailGun. Default: api.mailgun.net
- --mailgunKey The API key used to send emails with MailGun. If not specified, no emails will be sent. 
- --emailDomain The source domain to use when sending emails with MailGun. Default: api.ci-connect.net
- --config A path to a file containing further configuration settings specified one per line as `option_name=option_value` pairs. This option may be used repeatedly to read multiple configuration files, in which case options specified in later files individually supercede previous specification of the same options in other files, as command line arguments, or as environment variables. 

## The 'Bootstrap User File'

This file is used when the the API server starts up to set the properties of the root user. 
It must contain, separated by newlines:

- The root user's name, which is typically `root`
- The root user's email address
- The root user's phone number
- The root user's institution
- The root user's API token

With the exception of the institution, none of these fields may contain whitespace. The root API token is the fundamental credential from which all actions allowed by the API are ultimately authorized, so it must be kept secret. 