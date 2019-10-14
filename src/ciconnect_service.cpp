#include <cerrno>
#include <iostream>
#include <cctype>

#include <sys/stat.h>

#define CROW_ENABLE_SSL
#include <crow.h>

#include "Entities.h"
#include "Logging.h"
#include "PersistentStore.h"
// #include "Process.h"
#include "ServerUtilities.h"

#include "UserCommands.h"
#include "GroupCommands.h"
#include "VersionCommands.h"

struct Configuration{
	struct ParamRef{
		enum Type{String,Bool} type;
		union{
			std::reference_wrapper<std::string> s;
			std::reference_wrapper<bool> b;
		};
		ParamRef(std::string& s):type(String),s(s){}
		ParamRef(bool& b):type(Bool),b(b){}
		ParamRef(const ParamRef& p):type(p.type){
			switch(type){
				case String: s=p.s; break;
				case Bool: b=p.b; break;
			}
		}
		
		ParamRef& operator=(const std::string& value){
			switch(type){
				case String:
					s.get()=value;
					break;
				case Bool:
				{
					 if(value=="true" || value=="True" || value=="1")
					 	b.get()=true;
					 else
					 	b.get()=false;
					 break;
				}
			}
			return *this;
		}
	};

	std::string awsAccessKey;
	std::string awsSecretKey;
	std::string awsRegion;
	std::string awsURLScheme;
	std::string awsEndpoint;
	std::string portString;
	std::string sslCertificate;
	std::string sslKey;
	std::string bootstrapUserFile;
	std::string mailgunEndpoint;
	std::string mailgunKey;
	std::string emailDomain;
	
	std::map<std::string,ParamRef> options;
	
	Configuration(int argc, char* argv[]):
	awsAccessKey("foo"),
	awsSecretKey("bar"),
	awsRegion("us-east-1"),
	awsURLScheme("http"),
	awsEndpoint("localhost:8000"),
	portString("18080"),
	bootstrapUserFile("base_connect_user"),
	mailgunEndpoint("api.mailgun.net"),
	emailDomain("api.ci-connect.net"),
	options{
		{"awsAccessKey",awsAccessKey},
		{"awsSecretKey",awsSecretKey},
		{"awsRegion",awsRegion},
		{"awsURLScheme",awsURLScheme},
		{"awsEndpoint",awsEndpoint},
		{"port",portString},
		{"sslCertificate",sslCertificate},
		{"sslKey",sslKey},
		{"bootstrapUserFile",bootstrapUserFile},
		{"mailgunEndpoint",mailgunEndpoint},
		{"mailgunKey",mailgunKey},
		{"emailDomain",emailDomain}
	}
	{
		//check for environment variables
		for(auto& option : options)
			fetchFromEnvironment("CICONNECT_"+option.first,option.second);
		
		std::string configPath;
		fetchFromEnvironment("CICONNECT_config",configPath);
		if(!configPath.empty())
			parseFile({configPath});
		
		//interpret command line arguments
		for(int i=1; i<argc; i++){
			std::string arg(argv[i]);
			if(arg.size()<=2 || arg[0]!='-' || arg[1]!='-'){
				log_error("Unknown argument ignored: '" << arg << '\'');
				continue;
			}
			auto eqPos=arg.find('=');
			std::string optName=arg.substr(2,eqPos-2);
			if(options.count(optName)){
				if(eqPos!=std::string::npos)
					options.find(optName)->second=arg.substr(eqPos+1);
				else{
					if(i==argc-1)
						log_fatal("Missing value after "+arg);
					i++;
					options.find(arg.substr(2))->second=argv[i];
				}
			}
			else if(optName=="config"){
				if(eqPos!=std::string::npos)
					parseFile({arg.substr(eqPos+1)});
				else{
					if(i==argc-1)
						log_fatal("Missing value after "+arg);
					i++;
					parseFile({argv[i]});
				}
			}
			else
				log_error("Unknown argument ignored: '" << arg << '\'');
		}
	}
	
	//attempt to read the last file in files, checking that it does not appear
	//previously
	void parseFile(const std::vector<std::string>& files){
		assert(!files.empty());
		if(std::find(files.begin(),files.end(),files.back())<(files.end()-1)){
			log_error("Configuration file loop: ");
			for(const auto file : files)
				log_error("  " << file);
			log_fatal("Configuration parsing terminated");
		}
		std::ifstream infile(files.back());
		if(!infile)
			log_fatal("Unable to open " << files.back() << " for reading");
		std::string line;
		unsigned int lineNumber=1;
		while(std::getline(infile,line)){
			auto eqPos=line.find('=');
			std::string optName=line.substr(0,eqPos);
			std::string value=line.substr(eqPos+1);
			if(options.count(optName))
				options.find(optName)->second=value;
			else if(optName=="config"){
				auto newFiles=files;
				newFiles.push_back(value);
				parseFile(newFiles);
			}
			else
				log_error(files.back() << ':' << lineNumber 
				          << ": Unknown option ignored: '" << line << '\'');
			lineNumber++;
		}
	}
	
};

///Accept a dictionary describing several individual requests, execute them all 
///concurrently, and return the results in another dictionary. Currently very
///simplistic; a new thread will be spawned for every individual request. 
crow::response multiplex(crow::SimpleApp& server, PersistentStore& store, const crow::request& req){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested execute a command bundle");
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	
	rapidjson::Document body;
	try{
		body.Parse(req.body.c_str());
	}catch(std::runtime_error& err){
		return crow::response(400,generateError("Invalid JSON in request body"));
	}
	
	if(!body.IsObject())
		return crow::response(400,generateError("Multiplexed requests must have a JSON object/dictionary as the request body"));
	
	auto parseHTTPMethod=[](std::string method){
		std::transform(method.begin(),method.end(),method.begin(),[](char c)->char{return std::toupper(c);});
		if(method=="DELETE") return crow::HTTPMethod::Delete;
		if(method=="GET") return crow::HTTPMethod::Get;
		if(method=="HEAD") return crow::HTTPMethod::Head;
		if(method=="POST") return crow::HTTPMethod::Post;
		if(method=="PUT") return crow::HTTPMethod::Put;
		if(method=="CONNECT") return crow::HTTPMethod::Connect;
		if(method=="OPTIONS") return crow::HTTPMethod::Options;
		if(method=="TRACE") return crow::HTTPMethod::Trace;
		if(method=="PATCH") return crow::HTTPMethod::Patch;
		if(method=="PURGE") return crow::HTTPMethod::Purge;
		throw std::runtime_error(generateError("Unrecognized HTTP method: "+method));
	};
	
	std::vector<crow::request> requests;
	requests.reserve(body.GetObject().MemberCount());
	for(const auto& rawRequest : body.GetObject()){
		if(!rawRequest.value.IsObject())
			return crow::response(400,generateError("Individual requests must be represented as JSON objects/dictionaries"));
		if(!rawRequest.value.HasMember("method") || !rawRequest.value["method"].IsString())
			return crow::response(400,generateError("Individual requests must have a string member named 'method' indicating the HTTP method"));
		if(rawRequest.value.HasMember("body") && !rawRequest.value["method"].IsString())
			return crow::response(400,generateError("Individual requests must have bodies represented as strings"));
		std::string rawURL=rawRequest.name.GetString();
		std::string body;
		if(rawRequest.value.HasMember("body"))
			body=rawRequest.value["body"].GetString();
		requests.emplace_back(parseHTTPMethod(rawRequest.value["method"].GetString()), //method
		                      rawURL, //raw_url
		                      rawURL.substr(0, rawURL.find("?")), //url
		                      crow::query_string(rawURL), //url_params
		                      crow::ci_map{}, //headers, currently not handled
		                      body //body
		                      );
	}
	
	std::vector<std::future<crow::response>> responses;
	responses.reserve(requests.size());
	
	for(const auto& request : requests)
		responses.emplace_back(std::async(std::launch::deferred,[&](){ 
			crow::response response;
			server.handle(request, response);
			return response;
		}));
	
	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	for(std::size_t i=0; i<requests.size(); i++){
		const auto& request=requests[i];
		rapidjson::Value singleResult(rapidjson::kObjectType);
		try{
			crow::response response=responses[i].get();
			singleResult.AddMember("status",response.code,alloc);
			singleResult.AddMember("body",response.body,alloc);
		}
		catch(std::exception& ex){
			singleResult.AddMember("status",400,alloc);
			singleResult.AddMember("body",generateError(ex.what()),alloc);
		}
		catch(...){
			singleResult.AddMember("status",400,alloc);
			singleResult.AddMember("body",generateError("Exception"),alloc);
		}
		rapidjson::Value key(rapidjson::kStringType);
		key.SetString(requests[i].raw_url, alloc);
		result.AddMember(key, singleResult, alloc);
	}
	
	return crow::response(to_string(result));
}

int main(int argc, char* argv[]){
	Configuration config(argc, argv);
	
	if(config.sslCertificate.empty()!=config.sslKey.empty()){
		log_fatal("--sslCertificate ($CICONNECT_sslCertificate) and --sslKey ($CICONNECT_sslKey)"
		          " must be specified together");
	}
	
	log_info("Database URL is " << config.awsURLScheme << "://" << config.awsEndpoint);
	unsigned int port=0;
	{
		std::istringstream is(config.portString);
		is >> port;
		if(!port || is.fail())
			log_fatal("Unable to parse \"" << config.portString << "\" as a valid port number");
	}
	log_info("Service port is " << port);
	
	//startReaper();
	// DB client initialization
	Aws::SDKOptions awsOptions;
	Aws::InitAPI(awsOptions);
	using AWSOptionsHandle=std::unique_ptr<Aws::SDKOptions,void(*)(Aws::SDKOptions*)>;
	AWSOptionsHandle opt_holder(&awsOptions,
								[](Aws::SDKOptions* awsOptions){
									Aws::ShutdownAPI(*awsOptions); 
								});
	Aws::Auth::AWSCredentials credentials(config.awsAccessKey,config.awsSecretKey);
	Aws::Client::ClientConfiguration clientConfig;
	clientConfig.region=config.awsRegion;
	if(config.awsURLScheme=="http")
		clientConfig.scheme=Aws::Http::Scheme::HTTP;
	else if(config.awsURLScheme=="https")
		clientConfig.scheme=Aws::Http::Scheme::HTTPS;
	else
		log_fatal("Unrecognized URL scheme for AWS: '" << config.awsURLScheme << '\'');
	clientConfig.endpointOverride=config.awsEndpoint;
	
	EmailClient emailClient(config.mailgunEndpoint,config.mailgunKey,config.emailDomain);
	
	PersistentStore store(credentials,clientConfig,
	                      config.bootstrapUserFile,
	                      emailClient);
	
	// REST server initialization
	crow::SimpleApp server;
	
	CROW_ROUTE(server, "/v1alpha1/multiplex").methods("POST"_method)(
	  [&](const crow::request& req){ return multiplex(server,store,req); });
	
	// == User commands ==
	CROW_ROUTE(server, "/v1alpha1/users").methods("GET"_method)(
	  [&](const crow::request& req){ return listUsers(store,req); });
	CROW_ROUTE(server, "/v1alpha1/users").methods("POST"_method)(
	  [&](const crow::request& req){ return createUser(store,req); });
	CROW_ROUTE(server, "/v1alpha1/users/<string>").methods("GET"_method)(
	  [&](const crow::request& req, const std::string& uID){ return getUserInfo(store,req,uID); });
	CROW_ROUTE(server, "/v1alpha1/users/<string>").methods("PUT"_method)(
	  [&](const crow::request& req, const std::string& uID){ return updateUser(store,req,uID); });
	CROW_ROUTE(server, "/v1alpha1/users/<string>").methods("DELETE"_method)(
	  [&](const crow::request& req, const std::string& uID){ return deleteUser(store,req,uID); });
	CROW_ROUTE(server, "/v1alpha1/users/<string>/groups").methods("GET"_method)(
	  [&](const crow::request& req, const std::string& uID){ return listUserGroups(store,req,uID); });
	CROW_ROUTE(server, "/v1alpha1/users/<string>/groups/<string>").methods("GET"_method)(
	  [&](const crow::request& req, const std::string& userID, const std::string& groupID){ return getGroupMemberStatus(store,req,userID,groupID); });
	CROW_ROUTE(server, "/v1alpha1/users/<string>/groups/<string>").methods("PUT"_method)(
	  [&](const crow::request& req, const std::string& uID, const std::string groupID){ return setUserStatusInGroup(store,req,uID,groupID); });
	CROW_ROUTE(server, "/v1alpha1/users/<string>/groups/<string>").methods("DELETE"_method)(
	  [&](const crow::request& req, const std::string& uID, const std::string groupID){ return removeUserFromGroup(store,req,uID,groupID); });
	CROW_ROUTE(server, "/v1alpha1/users/<string>/group_requests").methods("GET"_method)(
	  [&](const crow::request& req, const std::string& uID){ return listUserGroupRequests(store,req,uID); });
	CROW_ROUTE(server, "/v1alpha1/users/<string>/attributes/<string>").methods("GET"_method)(
	  [&](const crow::request& req, const std::string& uID, const std::string& attr){ return getUserAttribute(store,req,uID,attr); });
	CROW_ROUTE(server, "/v1alpha1/users/<string>/attributes/<string>").methods("PUT"_method)(
	  [&](const crow::request& req, const std::string& uID, const std::string& attr){ return setUserAttribute(store,req,uID,attr); });
	CROW_ROUTE(server, "/v1alpha1/users/<string>/attributes/<string>").methods("DELETE"_method)(
	  [&](const crow::request& req, const std::string& uID, const std::string& attr){ return deleteUserAttribute(store,req,uID,attr); });
	CROW_ROUTE(server, "/v1alpha1/users/<string>/replace_token").methods("GET"_method)(
	  [&](const crow::request& req, const std::string& uID){ return replaceUserToken(store,req,uID); });
	CROW_ROUTE(server, "/v1alpha1/users/<string>/update_last_use_time").methods("PUT"_method)(
	  [&](const crow::request& req, const std::string& uID){ return updateLastUseTime(store,req,uID); });
	CROW_ROUTE(server, "/v1alpha1/find_user").methods("GET"_method)(
	  [&](const crow::request& req){ return findUser(store,req); });
	CROW_ROUTE(server, "/v1alpha1/check_unix_name").methods("GET"_method)(
	  [&](const crow::request& req){ return checkUnixName(store,req); });
	
	// == Group commands ==
	CROW_ROUTE(server, "/v1alpha1/groups").methods("GET"_method)(
	  [&](const crow::request& req){ return listGroups(store,req); });
	CROW_ROUTE(server, "/v1alpha1/groups/<string>").methods("GET"_method)(
	  [&](const crow::request& req, const std::string& groupID){ return getGroupInfo(store,req,groupID); });
	CROW_ROUTE(server, "/v1alpha1/groups/<string>").methods("PUT"_method)(
	  [&](const crow::request& req, const std::string& groupID){ return updateGroup(store,req,groupID); });
	CROW_ROUTE(server, "/v1alpha1/groups/<string>").methods("DELETE"_method)(
	  [&](const crow::request& req, const std::string& groupID){ return deleteGroup(store,req,groupID); });
	CROW_ROUTE(server, "/v1alpha1/groups/<string>/members").methods("GET"_method)(
	  [&](const crow::request& req, const std::string& groupID){ return listGroupMembers(store,req,groupID); });
	CROW_ROUTE(server, "/v1alpha1/groups/<string>/members/<string>").methods("GET"_method)(
	  [&](const crow::request& req, const std::string& groupID, const std::string& userID){ return getGroupMemberStatus(store,req,userID,groupID); });
	CROW_ROUTE(server, "/v1alpha1/groups/<string>/members/<string>").methods("PUT"_method)(
	  [&](const crow::request& req, const std::string groupID, const std::string& uID){ return setUserStatusInGroup(store,req,uID,groupID); });
	CROW_ROUTE(server, "/v1alpha1/groups/<string>/members/<string>").methods("DELETE"_method)(
	  [&](const crow::request& req, const std::string groupID, const std::string& uID){ return removeUserFromGroup(store,req,uID,groupID); });
	CROW_ROUTE(server, "/v1alpha1/groups/<string>/subgroups").methods("GET"_method)(
	  [&](const crow::request& req, const std::string& groupID){ return getSubgroups(store,req,groupID); });
	CROW_ROUTE(server, "/v1alpha1/groups/<string>/subgroups/<string>").methods("PUT"_method)(
	  [&](const crow::request& req, const std::string& pGroup, const std::string& cGroup){ return createGroup(store,req,pGroup,cGroup); });
	CROW_ROUTE(server, "/v1alpha1/groups/<string>/subgroup_requests").methods("GET"_method)(
	  [&](const crow::request& req, const std::string& groupID){ return getSubgroupRequests(store,req,groupID); });
	CROW_ROUTE(server, "/v1alpha1/groups/<string>/subgroup_requests/<string>").methods("PUT"_method)(
	  [&](const crow::request& req, const std::string& pGroup, const std::string& cGroup){ return createGroup(store,req,pGroup,cGroup); });
	CROW_ROUTE(server, "/v1alpha1/groups/<string>/subgroup_requests/<string>").methods("DELETE"_method)(
	  [&](const crow::request& req, const std::string& pGroup, const std::string& cGroup){ return denySubgroupRequest(store,req,pGroup,cGroup); });
	CROW_ROUTE(server, "/v1alpha1/groups/<string>/subgroup_requests/<string>/approve").methods("PUT"_method)(
	  [&](const crow::request& req, const std::string& pGroup, const std::string& cGroup){ return approveSubgroupRequest(store,req,pGroup,cGroup); });
	CROW_ROUTE(server, "/v1alpha1/groups/<string>/attributes/<string>").methods("GET"_method)(
	  [&](const crow::request& req, const std::string& group, const std::string& attr){ return getGroupAttribute(store,req,group,attr); });
	CROW_ROUTE(server, "/v1alpha1/groups/<string>/attributes/<string>").methods("PUT"_method)(
	  [&](const crow::request& req, const std::string& group, const std::string& attr){ return setGroupAttribute(store,req,group,attr); });
	CROW_ROUTE(server, "/v1alpha1/groups/<string>/attributes/<string>").methods("DELETE"_method)(
	  [&](const crow::request& req, const std::string& group, const std::string& attr){ return deleteGroupAttribute(store,req,group,attr); });
	CROW_ROUTE(server, "/v1alpha1/fields_of_science").methods("GET"_method)(
	  [&](const crow::request& req){ return getScienceFields(store,req); });
	
	
	CROW_ROUTE(server, "/v1alpha1/stats").methods("GET"_method)(
	  [&](){ return(store.getStatistics()); });
	
	//CROW_ROUTE(server, "/version").methods("GET"_method)(&serverVersionInfo);
	
	//include a fallback to catch unexpected/unsupported things
	CROW_ROUTE(server, "/<string>/<path>").methods("GET"_method)(
	  [](std::string apiVersion, std::string path){
	  	return crow::response(400,generateError("Unsupported API version")); });
	
	server.loglevel(crow::LogLevel::Warning);
	if(!config.sslCertificate.empty())
		server.port(port).ssl_file(config.sslCertificate,config.sslKey).multithreaded().run();
	else
		server.port(port).multithreaded().run();
}
