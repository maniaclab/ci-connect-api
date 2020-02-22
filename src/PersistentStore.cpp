#include <PersistentStore.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>

#include <unistd.h>

#include <boost/lexical_cast.hpp>

#include <aws/core/utils/Outcome.h>
#include <aws/dynamodb/model/DeleteItemRequest.h>
#include <aws/dynamodb/model/GetItemRequest.h>
#include <aws/dynamodb/model/PutItemRequest.h>
#include <aws/dynamodb/model/QueryRequest.h>
#include <aws/dynamodb/model/ScanRequest.h>
#include <aws/dynamodb/model/UpdateItemRequest.h>
#include <aws/dynamodb/model/TransactWriteItemsRequest.h>

#include <aws/dynamodb/model/CreateGlobalSecondaryIndexAction.h>
#include <aws/dynamodb/model/CreateTableRequest.h>
#include <aws/dynamodb/model/DeleteTableRequest.h>
#include <aws/dynamodb/model/DescribeTableRequest.h>
#include <aws/dynamodb/model/UpdateTableRequest.h>

#include <HTTPRequests.h>
#include <Logging.h>
#include <ServerUtilities.h>

namespace{
	
bool hasIndex(const Aws::DynamoDB::Model::TableDescription& tableDesc, const std::string& name){
	using namespace Aws::DynamoDB::Model;
	const Aws::Vector<GlobalSecondaryIndexDescription>& indices=tableDesc.GetGlobalSecondaryIndexes();
	return std::find_if(indices.begin(),indices.end(),
						[&name](const GlobalSecondaryIndexDescription& gsid)->bool{
							return gsid.GetIndexName()==name;
						})!=indices.end();
}
	
bool indexHasNonKeyProjection(const Aws::DynamoDB::Model::TableDescription& tableDesc, 
                              const std::string& index, const std::string& attr){
	using namespace Aws::DynamoDB::Model;
	const Aws::Vector<GlobalSecondaryIndexDescription>& indices=tableDesc.GetGlobalSecondaryIndexes();
	auto indexIt=std::find_if(indices.begin(),indices.end(),
	                          [&index](const GlobalSecondaryIndexDescription& gsid)->bool{
	                          	return gsid.GetIndexName()==index;
	                          });
	if(indexIt==indices.end())
		return false;
	for(const auto& attr_ : indexIt->GetProjection().GetNonKeyAttributes()){
		if(attr_==attr)
			return true;
	}
	return false;
}

Aws::DynamoDB::Model::CreateGlobalSecondaryIndexAction
secondaryIndexToCreateAction(const Aws::DynamoDB::Model::GlobalSecondaryIndex& index){
	using namespace Aws::DynamoDB::Model;
	Aws::DynamoDB::Model::CreateGlobalSecondaryIndexAction createAction;
	createAction
	.WithIndexName(index.GetIndexName())
	.WithKeySchema(index.GetKeySchema())
	.WithProjection(index.GetProjection())
	.WithProvisionedThroughput(index.GetProvisionedThroughput());
	return createAction;
}

Aws::DynamoDB::Model::UpdateTableRequest
updateTableWithNewSecondaryIndex(const std::string& tableName, const Aws::DynamoDB::Model::GlobalSecondaryIndex& index){
	using namespace Aws::DynamoDB::Model;
	auto request=UpdateTableRequest();
	request.SetTableName(tableName);
	request.AddGlobalSecondaryIndexUpdates(GlobalSecondaryIndexUpdate()
	                                       .WithCreate(secondaryIndexToCreateAction(index)));
	return request;
}
	
void waitTableReadiness(Aws::DynamoDB::DynamoDBClient& dbClient, const std::string& tableName){
	using namespace Aws::DynamoDB::Model;
	log_info("Waiting for table " << tableName << " to reach active status");
	DescribeTableOutcome outcome;
	do{
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		outcome=dbClient.DescribeTable(DescribeTableRequest()
		                               .WithTableName(tableName));
	}while(outcome.IsSuccess() && 
		   outcome.GetResult().GetTable().GetTableStatus()!=TableStatus::ACTIVE);
	if(!outcome.IsSuccess())
		log_fatal("Table " << tableName << " does not seem to be available? "
				  "Dynamo error: " << outcome.GetError().GetMessage());
}

void waitIndexReadiness(Aws::DynamoDB::DynamoDBClient& dbClient, 
                        const std::string& tableName, 
                        const std::string& indexName){
	using namespace Aws::DynamoDB::Model;
	log_info("Waiting for index " << indexName << " of table " << tableName << " to reach active status");
	DescribeTableOutcome outcome;
	using GSID=GlobalSecondaryIndexDescription;
	Aws::Vector<GSID> indices;
	Aws::Vector<GSID>::iterator index;
	do{
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		outcome=dbClient.DescribeTable(DescribeTableRequest()
		                               .WithTableName(tableName));
	}while(outcome.IsSuccess() && (
		   (indices=outcome.GetResult().GetTable().GetGlobalSecondaryIndexes()).empty() ||
		   (index=std::find_if(indices.begin(),indices.end(),[&](const GSID& id){ return id.GetIndexName()==indexName; }))==indices.end() ||
		   index->GetIndexStatus()!=IndexStatus::ACTIVE));
	if(!outcome.IsSuccess())
		log_fatal("Table " << tableName << " does not seem to be available? "
				  "Dynamo error: " << outcome.GetError().GetMessage());
}
	


void waitUntilIndexDeleted(Aws::DynamoDB::DynamoDBClient& dbClient, 
                        const std::string& tableName, 
                        const std::string& indexName){
	using namespace Aws::DynamoDB::Model;
	log_info("Waiting for index " << indexName << " of table " << tableName << " to be deleted");
	DescribeTableOutcome outcome;
	using GSID=GlobalSecondaryIndexDescription;
	Aws::Vector<GSID> indices;
	Aws::Vector<GSID>::iterator index;
	do{
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		outcome=dbClient.DescribeTable(DescribeTableRequest()
		                               .WithTableName(tableName));
	}while(outcome.IsSuccess() &&
		   !(indices=outcome.GetResult().GetTable().GetGlobalSecondaryIndexes()).empty() &&
		   (index=std::find_if(indices.begin(),indices.end(),[&](const GSID& id){ return id.GetIndexName()==indexName; }))!=indices.end());
	if(!outcome.IsSuccess())
		log_fatal("Table " << tableName << " does not seem to be available? "
				  "Dynamo error: " << outcome.GetError().GetMessage());
}

///A default string value to use in place of missing properties, when having a 
///trivial value is not a big concern
const Aws::DynamoDB::Model::AttributeValue missingString(" ");

template<typename Cache, typename Key=typename Cache::key_type, typename Value=typename Cache::mapped_type>
void replaceCacheRecord(Cache& cache, const Key& key, const Value& value){
	cache.upsert(key,[&value](Value& existing){ existing=value; },value);
}

} //anonymous namespace

EmailClient::EmailClient(const std::string& mailgunEndpoint, 
                         const std::string& mailgunKey, 
                         const std::string& emailDomain):
mailgunEndpoint(mailgunEndpoint),mailgunKey(mailgunKey),emailDomain(emailDomain){
	valid=!mailgunEndpoint.empty() && !mailgunKey.empty() && !emailDomain.empty();
	if(!valid)
		log_warn("Email settings are not valid; email notifications will be disabled");
}

bool EmailClient::sendEmail(const Email& email){
	if(!valid)
		return false;
	std::string url="https://api:"+mailgunKey+"@"+mailgunEndpoint+"/v3/"+emailDomain+"/messages";
	std::multimap<std::string,std::string> data{
		{"from",email.fromAddress},
		{"subject",email.subject},
		{"text",email.body}
	};
	for(const auto& to : email.toAddresses)
		data.emplace("to",to);
	for(const auto& cc : email.ccAddresses)
		data.emplace("cc",cc);
	for(const auto& bcc : email.bccAddresses)
		data.emplace("bcc",bcc);
	if(!email.replyTo.empty())
		data.emplace("h:Reply-To",email.replyTo);
	auto response=httpRequests::httpPostForm(url,data);
	if(response.status!=200){
		log_warn("Failed to send email: " << response.body);
		return false;
	}
	return true;
}

const unsigned int PersistentStore::minimumUserID=10000;
const unsigned int PersistentStore::maximumUserID=1u<<17;
const unsigned int PersistentStore::minimumGroupID=5000;
const unsigned int PersistentStore::maximumGroupID=1u<<17;
const std::string PersistentStore::nextIDKeyName="!_NextUnixID";

PersistentStore::PersistentStore(const Aws::Auth::AWSCredentials& credentials, 
                                 const Aws::Client::ClientConfiguration& clientConfig,
                                 std::string bootstrapUserFile, EmailClient emailClient):
	dbClient(credentials,clientConfig),
	userTableName("CONNECT_users"),
	groupTableName("CONNECT_groups"),
	emailClient(emailClient),
	userCacheValidity(std::chrono::minutes(60)),
	userCacheExpirationTime(std::chrono::steady_clock::now()),
	groupCacheValidity(std::chrono::minutes(60)),
	groupCacheExpirationTime(std::chrono::steady_clock::now()),
	groupRequestCacheExpirationTime(std::chrono::steady_clock::now()),
	cacheHits(0),databaseQueries(0),databaseScans(0)
{
	log_info("Starting database client");
	InitializeTables(bootstrapUserFile);
	log_info("Database client ready");
}

void PersistentStore::InitializeUserTable(){
	using namespace Aws::DynamoDB::Model;
	using AttDef=Aws::DynamoDB::Model::AttributeDefinition;
	using SAT=Aws::DynamoDB::Model::ScalarAttributeType;
	
	//{"ID","name","email","phone","institution","token","globusID","sshKey","superuser","serviceAccount"}
	
	//define indices
	auto getByTokenIndex=[](){
		return GlobalSecondaryIndex()
		       .WithIndexName("ByToken")
		       .WithKeySchema({KeySchemaElement()
		                       .WithAttributeName("token")
		                       .WithKeyType(KeyType::HASH)})
		       .WithProjection(Projection()
		                       .WithProjectionType(ProjectionType::INCLUDE)
		                       .WithNonKeyAttributes({"unixName"}))
		       .WithProvisionedThroughput(ProvisionedThroughput()
		                                  .WithReadCapacityUnits(1)
		                                  .WithWriteCapacityUnits(1));
	};
	auto getByGlobusIDIndex=[](){
		return GlobalSecondaryIndex()
		       .WithIndexName("ByGlobusID")
		       .WithKeySchema({KeySchemaElement()
		                       .WithAttributeName("globusID")
		                       .WithKeyType(KeyType::HASH)})
		       .WithProjection(Projection()
		                       .WithProjectionType(ProjectionType::INCLUDE)
		                       .WithNonKeyAttributes({"unixName","token"}))
		       .WithProvisionedThroughput(ProvisionedThroughput()
		                                  .WithReadCapacityUnits(1)
		                                  .WithWriteCapacityUnits(1));
	};
	auto getByGroupIndex=[](){
		return GlobalSecondaryIndex()
		       .WithIndexName("ByGroup")
		       .WithKeySchema({KeySchemaElement()
		                       .WithAttributeName("groupName")
		                       .WithKeyType(KeyType::HASH)})
		       .WithProjection(Projection()
		                       .WithProjectionType(ProjectionType::INCLUDE)
		                       .WithNonKeyAttributes({"unixName","state","stateSetBy"}))
		       .WithProvisionedThroughput(ProvisionedThroughput()
		                                  .WithReadCapacityUnits(1)
		                                  .WithWriteCapacityUnits(1));
	};
	auto getByUnixIDIndex=[](){
		return GlobalSecondaryIndex()
		       .WithIndexName("ByUnixID")
		       .WithKeySchema({KeySchemaElement()
		                       .WithAttributeName("unixID")
		                       .WithKeyType(KeyType::HASH)})
		       .WithProjection(Projection()
		                       .WithProjectionType(ProjectionType::KEYS_ONLY))
		       .WithProvisionedThroughput(ProvisionedThroughput()
		                                  .WithReadCapacityUnits(1)
		                                  .WithWriteCapacityUnits(1));
	};
	
	//check status of the table
	auto userTableOut=dbClient.DescribeTable(DescribeTableRequest()
	                                         .WithTableName(userTableName));
	if(!userTableOut.IsSuccess() &&
	   userTableOut.GetError().GetErrorType()!=Aws::DynamoDB::DynamoDBErrors::RESOURCE_NOT_FOUND){
		log_fatal("Unable to connect to DynamoDB: "
		          << userTableOut.GetError().GetMessage());
	}
	if(!userTableOut.IsSuccess()){
		log_info("Users table does not exist; creating");
		auto request=CreateTableRequest();
		request.SetTableName(userTableName);
		request.SetAttributeDefinitions({
			AttDef().WithAttributeName("unixName").WithAttributeType(SAT::S),
			AttDef().WithAttributeName("sortKey").WithAttributeType(SAT::S),
			AttDef().WithAttributeName("token").WithAttributeType(SAT::S),
			AttDef().WithAttributeName("globusID").WithAttributeType(SAT::S),
			AttDef().WithAttributeName("groupName").WithAttributeType(SAT::S),
			AttDef().WithAttributeName("unixID").WithAttributeType(SAT::N)
		});
		request.SetKeySchema({
			KeySchemaElement().WithAttributeName("unixName").WithKeyType(KeyType::HASH),
			KeySchemaElement().WithAttributeName("sortKey").WithKeyType(KeyType::RANGE)
		});
		request.SetProvisionedThroughput(ProvisionedThroughput()
		                                 .WithReadCapacityUnits(1)
		                                 .WithWriteCapacityUnits(1));
		request.AddGlobalSecondaryIndexes(getByTokenIndex());
		request.AddGlobalSecondaryIndexes(getByGlobusIDIndex());
		request.AddGlobalSecondaryIndexes(getByGroupIndex());
		request.AddGlobalSecondaryIndexes(getByUnixIDIndex());
		
		auto createOut=dbClient.CreateTable(request);
		if(!createOut.IsSuccess())
			log_fatal("Failed to create user table: " + createOut.GetError().GetMessage());
		
		waitTableReadiness(dbClient,userTableName);
		
		{
			//Set the initial unixID
			{
				using Aws::DynamoDB::Model::AttributeValue;
				auto request=Aws::DynamoDB::Model::PutItemRequest()
				.WithTableName(userTableName)
				.WithItem({
					{"unixName",AttributeValue(nextIDKeyName)},
					{"sortKey",AttributeValue(nextIDKeyName)},
					{"next_unixID",AttributeValue().SetN(std::to_string(minimumUserID))}
				});
				auto outcome=dbClient.PutItem(request);
				if(!outcome.IsSuccess()){
					auto err=outcome.GetError();
					log_fatal("Failed to set initial user ID record: " << err.GetMessage());
				}
			}
			//Insert the root user account
			try{
				if(!addUser(rootUser))
					log_fatal("Failed to inject root user");
			}
			catch(...){
				log_error("Failed to inject root user; deleting users table");
				//Demolish the whole table again. This is technically overkill, but it ensures that
				//on the next start up this step will be run again (hpefully with better results).
				auto outc=dbClient.DeleteTable(Aws::DynamoDB::Model::DeleteTableRequest().WithTableName(userTableName));
				//If the table deletion fails it is still possible to get stuck on a restart, but 
				//it isn't clear what else could be done about such a failure. 
				if(!outc.IsSuccess())
					log_error("Failed to delete users table: " << outc.GetError().GetMessage());
				throw;
			}
		}
		log_info("Created users table");
	}
	else{ //table exists; check whether any indices are missing
		TableDescription tableDesc=userTableOut.GetResult().GetTable();
		
		//check whether any indices are out of date
		bool changed=false;
		
		//if an index was deleted, update the table description so we know to recreate it
		if(changed){
			userTableOut=dbClient.DescribeTable(DescribeTableRequest()
			                                  .WithTableName(userTableName));
			tableDesc=userTableOut.GetResult().GetTable();
		}
		
		if(!hasIndex(tableDesc,"ByToken")){
			auto request=updateTableWithNewSecondaryIndex(userTableName,getByTokenIndex());
			request.WithAttributeDefinitions({AttDef().WithAttributeName("token").WithAttributeType(SAT::S)});
			auto createOut=dbClient.UpdateTable(request);
			if(!createOut.IsSuccess())
				log_fatal("Failed to add by-token index to user table: " + createOut.GetError().GetMessage());
			waitIndexReadiness(dbClient,userTableName,"ByToken");
			log_info("Added by-token index to user table");
		}
		if(!hasIndex(tableDesc,"ByGlobusID")){
			auto request=updateTableWithNewSecondaryIndex(userTableName,getByGlobusIDIndex());
			request.WithAttributeDefinitions({AttDef().WithAttributeName("globusID").WithAttributeType(SAT::S)});
			auto createOut=dbClient.UpdateTable(request);
			if(!createOut.IsSuccess())
				log_fatal("Failed to add by-GlobusID index to user table: " + createOut.GetError().GetMessage());
			waitIndexReadiness(dbClient,userTableName,"ByGlobusID");
			log_info("Added by-GlobusID index to user table");
		}
		if(!hasIndex(tableDesc,"ByGroup")){
			auto request=updateTableWithNewSecondaryIndex(userTableName,getByGroupIndex());
			request.WithAttributeDefinitions({AttDef().WithAttributeName("groupName").WithAttributeType(SAT::S)});
			auto createOut=dbClient.UpdateTable(request);
			if(!createOut.IsSuccess())
				log_fatal("Failed to add by-Group index to user table: " + createOut.GetError().GetMessage());
			waitIndexReadiness(dbClient,userTableName,"ByGroup");
			log_info("Added by-Group index to user table");
		}
		if(!hasIndex(tableDesc,"ByUnixID")){
			auto request=updateTableWithNewSecondaryIndex(userTableName,getByUnixIDIndex());
			request.WithAttributeDefinitions({AttDef().WithAttributeName("unixID").WithAttributeType(SAT::S)});
			auto createOut=dbClient.UpdateTable(request);
			if(!createOut.IsSuccess())
				log_fatal("Failed to add by-UnixID index to user table: " + createOut.GetError().GetMessage());
			waitIndexReadiness(dbClient,userTableName,"ByUnixID");
			log_info("Added by-UnixID index to user table");
		}
	}
}

void PersistentStore::InitializeGroupTable(){
	using namespace Aws::DynamoDB::Model;
	using AttDef=Aws::DynamoDB::Model::AttributeDefinition;
	using SAT=Aws::DynamoDB::Model::ScalarAttributeType;
	
	//define indices
	auto getByUnixIDIndex=[](){
		return GlobalSecondaryIndex()
		       .WithIndexName("ByUnixID")
		       .WithKeySchema({KeySchemaElement()
		                       .WithAttributeName("unixID")
		                       .WithKeyType(KeyType::HASH)})
		       .WithProjection(Projection()
		                       .WithProjectionType(ProjectionType::KEYS_ONLY))
		       .WithProvisionedThroughput(ProvisionedThroughput()
		                                  .WithReadCapacityUnits(1)
		                                  .WithWriteCapacityUnits(1));
	};
	auto getByRequesterIndex=[](){
		return GlobalSecondaryIndex()
		       .WithIndexName("ByRequester")
		       .WithKeySchema({KeySchemaElement()
		                       .WithAttributeName("requester")
		                       .WithKeyType(KeyType::HASH)})
		       .WithProjection(Projection()
		                       .WithProjectionType(ProjectionType::KEYS_ONLY))
		       .WithProvisionedThroughput(ProvisionedThroughput()
		                                  .WithReadCapacityUnits(1)
		                                  .WithWriteCapacityUnits(1));
	};
	
	//check status of the table
	auto groupTableOut=dbClient.DescribeTable(DescribeTableRequest()
											 .WithTableName(groupTableName));
	if(!groupTableOut.IsSuccess() &&
	   groupTableOut.GetError().GetErrorType()!=Aws::DynamoDB::DynamoDBErrors::RESOURCE_NOT_FOUND){
		log_fatal("Unable to connect to DynamoDB: "
		          << groupTableOut.GetError().GetMessage());
	}
	if(!groupTableOut.IsSuccess()){
		log_info("groups table does not exist; creating");
		auto request=CreateTableRequest();
		request.SetTableName(groupTableName);
		request.SetAttributeDefinitions({
			AttDef().WithAttributeName("name").WithAttributeType(SAT::S),
			AttDef().WithAttributeName("sortKey").WithAttributeType(SAT::S),
			AttDef().WithAttributeName("unixID").WithAttributeType(SAT::N),
			AttDef().WithAttributeName("requester").WithAttributeType(SAT::S)
		});
		request.SetKeySchema({
			KeySchemaElement().WithAttributeName("name").WithKeyType(KeyType::HASH),
			KeySchemaElement().WithAttributeName("sortKey").WithKeyType(KeyType::RANGE)
		});
		request.SetProvisionedThroughput(ProvisionedThroughput()
		                                 .WithReadCapacityUnits(1)
		                                 .WithWriteCapacityUnits(1));
		request.AddGlobalSecondaryIndexes(getByUnixIDIndex());
		request.AddGlobalSecondaryIndexes(getByRequesterIndex());
		
		auto createOut=dbClient.CreateTable(request);
		if(!createOut.IsSuccess())
			log_fatal("Failed to create groups table: " + createOut.GetError().GetMessage());
		
		waitTableReadiness(dbClient,groupTableName);
		
		{
			//Set the initial unixID
			{
				using Aws::DynamoDB::Model::AttributeValue;
				auto request=Aws::DynamoDB::Model::PutItemRequest()
				.WithTableName(groupTableName)
				.WithItem({
					{"name",AttributeValue(nextIDKeyName)},
					{"sortKey",AttributeValue(nextIDKeyName)},
					{"next_unixID",AttributeValue().SetN(std::to_string(minimumGroupID))}
				});
				auto outcome=dbClient.PutItem(request);
				if(!outcome.IsSuccess()){
					auto err=outcome.GetError();
					log_fatal("Failed to set initial group ID record: " << err.GetMessage());
				}
			}
			//Insert the root group, and make the root user an admin of it
			try{
				Group rootGroup;
				rootGroup.name="root";
				rootGroup.displayName="Root Group";
				rootGroup.email="none";
				rootGroup.phone="none";
				rootGroup.purpose="ResourceProvider";
				rootGroup.description="Root group which contains all users but is associated with no resources";
				rootGroup.creationDate=timestamp();
				rootGroup.valid=true;
				if(!addGroup(rootGroup))
					log_fatal("Failed to inject root group");
				
				GroupMembership rootOwnership;
				rootOwnership.userName=rootUser.unixName;
				rootOwnership.groupName=rootGroup.name;
				rootOwnership.state=GroupMembership::Admin;
				rootOwnership.stateSetBy="user:"+rootUser.unixName;
				rootOwnership.valid=true;
				if(!setUserStatusInGroup(rootOwnership))
					log_fatal("Failed to inject root user into root group");
			}
			catch(...){
				log_error("Failed to inject root group; deleting group table");
				//Demolish the whole table again. This is technically overkill, but it ensures that
				//on the next start up this step will be run again (hpefully with better results).
				auto outc=dbClient.DeleteTable(Aws::DynamoDB::Model::DeleteTableRequest().WithTableName(groupTableName));
				//If the table deletion fails it is still possible to get stuck on a restart, but 
				//it isn't clear what else could be done about such a failure. 
				if(!outc.IsSuccess())
					log_error("Failed to delete group table: " << outc.GetError().GetMessage());
				throw;
			}
		}
		log_info("Created groups table");
	}
	else{ //table exists; check whether any indices are missing
		TableDescription tableDesc=groupTableOut.GetResult().GetTable();
		
		//check whether any indices are out of date
		bool changed=false;
		
		//if an index was deleted, update the table description so we know to recreate it
		if(changed){
			groupTableOut=dbClient.DescribeTable(DescribeTableRequest()
			                                  .WithTableName(groupTableName));
			tableDesc=groupTableOut.GetResult().GetTable();
		}
		
		//add indices
		if(!hasIndex(tableDesc,"ByUnixID")){
			auto request=updateTableWithNewSecondaryIndex(groupTableName,getByUnixIDIndex());
			request.WithAttributeDefinitions({AttDef().WithAttributeName("unixID").WithAttributeType(SAT::S)});
			auto createOut=dbClient.UpdateTable(request);
			if(!createOut.IsSuccess())
				log_fatal("Failed to add by-UnixID index to group table: " + createOut.GetError().GetMessage());
			waitIndexReadiness(dbClient,groupTableName,"ByUnixID");
			log_info("Added by-UnixID index to group table");
		}
		if(!hasIndex(tableDesc,"ByRequester")){
			auto request=updateTableWithNewSecondaryIndex(groupTableName,getByUnixIDIndex());
			request.WithAttributeDefinitions({AttDef().WithAttributeName("requester").WithAttributeType(SAT::S)});
			auto createOut=dbClient.UpdateTable(request);
			if(!createOut.IsSuccess())
				log_fatal("Failed to add by-Requester index to group table: " + createOut.GetError().GetMessage());
			waitIndexReadiness(dbClient,groupTableName,"ByRequester");
			log_info("Added by-Requester index to group table");
		}
	}
}

void PersistentStore::InitializeTables(std::string bootstrapUserFile){
	{
		std::ifstream credFile(bootstrapUserFile);
		if(!credFile)
			log_fatal("Unable to read root user credentials");
		std::getline(credFile,rootUser.name);
		credFile >> rootUser.unixName >> rootUser.email 
				 >> rootUser.phone;
		credFile.ignore(1024,'\n');
		std::getline(credFile,rootUser.institution);
		credFile >> rootUser.token;
		if(credFile.fail())
			log_fatal("Unable to read root user credentials");
		rootUser.globusID="No Globus ID";
		rootUser.sshKey="No SSH key";
		rootUser.unixName="root";
		rootUser.joinDate=timestamp();
		rootUser.lastUseTime=timestamp();
		rootUser.superuser=true;
		rootUser.serviceAccount=true;
		rootUser.valid=true;
	}
	InitializeUserTable();
	InitializeGroupTable();
}

unsigned int PersistentStore::getNextIDHint(const std::string& tableName, 
                                            const std::string& nameKeyName){
	databaseQueries++;
	using Aws::DynamoDB::Model::AttributeValue;
	auto outcome=dbClient.GetItem(Aws::DynamoDB::Model::GetItemRequest()
								  .WithTableName(tableName)
								  .WithKey({{nameKeyName,AttributeValue(nextIDKeyName)},
											{"sortKey",AttributeValue(nextIDKeyName)}}));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_fatal("Failed to fetch unix ID record: " << err.GetMessage());
	}
	const auto& item=outcome.GetResult().GetItem();
	if(item.empty()) //no match found
		log_fatal(nextIDKeyName + " not found in " + tableName);
	return std::stoul(findOrThrow(item,"next_unixID","record missing next_unixID attribute").GetN());
}

bool PersistentStore::checkIDAvailability(const std::string& tableName, 
	                                      const std::string& nameKeyName,
	                                      unsigned int id){
	databaseQueries++;
	using Aws::DynamoDB::Model::AttributeValue;
	auto request=Aws::DynamoDB::Model::QueryRequest()
		.WithTableName(tableName)
		.WithIndexName("ByUnixID")
		.WithKeyConditionExpression("#id = :id_val")
		.WithExpressionAttributeNames({
			{"#id","unixID"}
		})
		.WithExpressionAttributeValues({
			{":id_val",AttributeValue().SetN(std::to_string(id))}
		});
	auto outcome=dbClient.Query(request);
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_fatal("Failed to fetch unix ID record: " << err.GetMessage());
	}
	const auto& queryResult=outcome.GetResult();
	return queryResult.GetCount()==0;
}

bool PersistentStore::reserveUnixID(const std::string& tableName, 
	               const std::string& nameKeyName,
	               unsigned int expected,
	               unsigned int id,
	               unsigned int next,
	               const std::string& recordName){
	using Aws::DynamoDB::Model::AttributeValue;
	using Aws::DynamoDB::Model::AttributeValueUpdate;
	using Aws::DynamoDB::Model::TransactWriteItem;
	
	auto request=Aws::DynamoDB::Model::TransactWriteItemsRequest()
	.WithTransactItems({
		TransactWriteItem().WithUpdate(
			Aws::DynamoDB::Model::Update()
			.WithTableName(tableName)
			.WithKey({{nameKeyName,AttributeValue(nextIDKeyName)},
					  {"sortKey",AttributeValue(nextIDKeyName)}})
			.WithUpdateExpression("SET #id = :new_val")
			.WithConditionExpression("#id = :old_val")
			.WithExpressionAttributeNames({
				{"#id","next_unixID"}
			})
			.WithExpressionAttributeValues({
				{":old_val",AttributeValue().SetN(std::to_string(expected))},
				{":new_val",AttributeValue().SetN(std::to_string(next))}
			})
		),
		TransactWriteItem().WithPut(
			Aws::DynamoDB::Model::Put()
			.WithTableName(tableName)
			.WithItem({
				{nameKeyName,AttributeValue(recordName)},
				{"sortKey",AttributeValue(recordName)},
				{"unixID",AttributeValue().SetN(std::to_string(id))}
			})
		),
	});
	auto outcome=dbClient.TransactWriteItems(request);
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to update next unix ID record: " << err.GetMessage());
		return false;
	}
	return true;
}

unsigned int PersistentStore::allocateUnixID(const std::string& tableName, const std::string& nameKeyName, const unsigned int minID, const unsigned int maxID, std::string recordName){
	auto readNext=[&]()->unsigned int{return getNextIDHint(tableName,nameKeyName);};
	
	unsigned int initialID=readNext();
	log_info("Next ID hint: " << initialID);
	unsigned int tryID=initialID;
	while(true){
		unsigned int nextID=tryID+1;
		if(nextID==maxID)
			nextID=minID;
		if(checkIDAvailability(tableName,nameKeyName,tryID)){
			//if(obtain(initialID,tryID,nextID))
			if(reserveUnixID(tableName,nameKeyName,initialID,tryID,nextID,recordName)){
				log_info("Allocated ID " << tryID);
				return tryID;
			}
		}
		unsigned int recommendedNext=readNext();
		if(recommendedNext==initialID){
			if(nextID==initialID)
				log_fatal("Unable to allocate numeric unix ID");
			tryID=nextID;
		}
		else{
			tryID=initialID=recommendedNext;
			log_info("Next ID hint: " << initialID);
		}
	}
}

bool PersistentStore::allocateSpecificUnixID(const std::string& tableName, 
const std::string& nameKeyName, const unsigned int minID, const unsigned int maxID, 
std::string recordName, unsigned int targetID){
	auto readNext=[&]()->unsigned int{return getNextIDHint(tableName,nameKeyName);};
	
	unsigned int initialID=readNext();
	while(true){
		if(!checkIDAvailability(tableName,nameKeyName,targetID))
			return false;
		if(reserveUnixID(tableName,nameKeyName,initialID,targetID,initialID,recordName))
			return true;
		unsigned int initialID=readNext();
	}
}

bool PersistentStore::addUser(User& user){
	if(user.unixID!=0){
		bool reserved=allocateSpecificUnixID(userTableName,"unixName",minimumUserID,maximumUserID,user.unixName,user.unixID);
		if(!reserved)
			log_fatal("Existing user ID (" << user.unixID << ") is already in use");
	}
	else
		user.unixID=allocateUnixID(userTableName,"unixName",minimumUserID,maximumUserID,user.unixName);

	using Aws::DynamoDB::Model::AttributeValue;
	auto request=Aws::DynamoDB::Model::PutItemRequest()
	.WithTableName(userTableName)
	.WithItem({
		{"unixName",AttributeValue(user.unixName)},
		{"sortKey",AttributeValue(user.unixName)},
		{"name",AttributeValue(user.name)},
		{"globusID",AttributeValue(user.globusID)},
		{"token",AttributeValue(user.token)},
		{"email",AttributeValue(user.email)},
		{"phone",AttributeValue(user.phone)},
		{"institution",AttributeValue(user.institution)},
		{"sshKey",AttributeValue(user.sshKey)},
		{"joinDate",AttributeValue(user.joinDate)},
		{"lastUseTime",AttributeValue(user.lastUseTime)},
		{"superuser",AttributeValue().SetBool(user.superuser)},
		{"serviceAccount",AttributeValue().SetBool(user.serviceAccount)},
		{"unixID",AttributeValue().SetN(std::to_string(user.unixID))}
	});
	auto outcome=dbClient.PutItem(request);
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to add user record: " << err.GetMessage());
		return false;
	}
	
	//update caches
	CacheRecord<User> record(user,userCacheValidity);
	replaceCacheRecord(userCache,user.unixName,record);
	replaceCacheRecord(userByTokenCache,user.token,record);
	replaceCacheRecord(userByGlobusIDCache,user.globusID,record);
	
	return true;
}

User PersistentStore::getUser(const std::string& id){
	//first see if we have this cached
	{
		CacheRecord<User> record;
		if(userCache.find(id,record)){
			//we have a cached record; is it still valid?
			if(record){ //it is, just return it
				cacheHits++;
				return record;
			}
		}
	}
	//need to query the database
	databaseQueries++;
	log_info("Querying database for user " << id);
	using Aws::DynamoDB::Model::AttributeValue;
	auto outcome=dbClient.GetItem(Aws::DynamoDB::Model::GetItemRequest()
								  .WithTableName(userTableName)
								  .WithKey({{"unixName",AttributeValue(id)},
	                                        {"sortKey",AttributeValue(id)}}));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to fetch user record: " << err.GetMessage());
		return User();
	}
	const auto& item=outcome.GetResult().GetItem();
	if(item.empty()) //no match found
		return User{};
	User user;
	user.valid=true;
	user.unixName=id;
	user.name=findOrThrow(item,"name","user record missing name attribute").GetS();
	user.email=findOrThrow(item,"email","user record missing email attribute").GetS();
	user.phone=findOrDefault(item,"phone",missingString).GetS();
	user.institution=findOrDefault(item,"institution",missingString).GetS();
	user.token=findOrThrow(item,"token","user record missing token attribute").GetS();
	user.globusID=findOrThrow(item,"globusID","user record missing globusID attribute").GetS();
	user.sshKey=findOrThrow(item,"sshKey","user record missing sshKey attribute").GetS();
	user.joinDate=findOrThrow(item,"joinDate","user record missing joinDate attribute").GetS();
	user.lastUseTime=findOrThrow(item,"lastUseTime","user record missing lastUseTime attribute").GetS();
	user.superuser=findOrThrow(item,"superuser","user record missing superuser attribute").GetBool();
	user.serviceAccount=findOrThrow(item,"serviceAccount","user record missing serviceAccount attribute").GetBool();
	user.unixID=std::stoul(findOrThrow(item,"unixID","user record missing unixID attribute (getUser)").GetN());
	
	//update caches
	CacheRecord<User> record(user,userCacheValidity);
	replaceCacheRecord(userCache,user.unixName,record);
	replaceCacheRecord(userByTokenCache,user.token,record);
	replaceCacheRecord(userByGlobusIDCache,user.globusID,record);
	
	return user;
}

User PersistentStore::findUserByToken(const std::string& token){
	//first see if we have this cached
	{
		CacheRecord<User> record;
		if(userByTokenCache.find(token,record)){
			//we have a cached record; is it still valid?
			if(record){ //it is, just return it
				cacheHits++;
				return record;
			}
		}
	}
	//need to query the database
	databaseQueries++;
	using Aws::DynamoDB::Model::AttributeValue;
	auto request=Aws::DynamoDB::Model::QueryRequest()
	.WithTableName(userTableName)
	.WithIndexName("ByToken")
	.WithKeyConditionExpression("#token = :tok_val")
	.WithExpressionAttributeNames({
		{"#token","token"}
	})
	.WithExpressionAttributeValues({
		{":tok_val",AttributeValue(token)}
	});
	auto outcome=dbClient.Query(request);
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to look up user by token: " << err.GetMessage());
		return User();
	}
	const auto& queryResult=outcome.GetResult();
	if(queryResult.GetCount()==0)
		return User();
	if(queryResult.GetCount()>1)
		log_fatal("Multiple user records are associated with token " << token << '!');
	
	const auto& item=queryResult.GetItems().front();
	return getUser(findOrThrow(item,"unixName","user record missing unixName attribute").GetS());
}

User PersistentStore::findUserByGlobusID(const std::string& globusID){
	//first see if we have this cached
	{
		CacheRecord<User> record;
		if(userByGlobusIDCache.find(globusID,record)){
			//we have a cached record; is it still valid?
			if(record){ //it is, just return it
				cacheHits++;
				return record;
			}
		}
	}
	//need to query the database
	databaseQueries++;
	using AV=Aws::DynamoDB::Model::AttributeValue;
	auto outcome=dbClient.Query(Aws::DynamoDB::Model::QueryRequest()
								.WithTableName(userTableName)
								.WithIndexName("ByGlobusID")
								.WithKeyConditionExpression("#globusID = :id_val")
								.WithExpressionAttributeNames({{"#globusID","globusID"}})
								.WithExpressionAttributeValues({{":id_val",AV(globusID)}})
								);
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to look up user by Globus ID: " << err.GetMessage());
		return User();
	}
	const auto& queryResult=outcome.GetResult();
	if(queryResult.GetCount()==0)
		return User();
	if(queryResult.GetCount()>1)
		log_fatal("Multiple user records are associated with Globus ID " << globusID << '!');
	
	const auto& item=queryResult.GetItems().front();
	User user;
	user.valid=true;
	user.unixName=findOrThrow(item,"unixName","user record missing unixName attribute").GetS();
	user.token=findOrThrow(item,"token","user record missing token attribute").GetS();
	user.globusID=globusID;
	
	//update caches
	CacheRecord<User> record(user,userCacheValidity);
	//We don't have enough information to populate the other caches. :(
	//replaceCacheRecord(userCache,user.unixName,record);
	//replaceCacheRecord(userByTokenCache,user.token,record);
	replaceCacheRecord(userByGlobusIDCache,user.globusID,record);
	
	return user;
}

bool PersistentStore::updateUser(const User& user, const User& oldUser){
	using AV=Aws::DynamoDB::Model::AttributeValue;
	using AVU=Aws::DynamoDB::Model::AttributeValueUpdate;
	auto outcome=dbClient.UpdateItem(Aws::DynamoDB::Model::UpdateItemRequest()
	                                 .WithTableName(userTableName)
									 .WithKey({{"unixName",AV(user.unixName)},
	                                           {"sortKey",AV(user.unixName)}})
	                                 .WithAttributeUpdates({
	                                            {"name",AVU().WithValue(AV(user.name))},
	                                            {"globusID",AVU().WithValue(AV(user.globusID))},
	                                            {"token",AVU().WithValue(AV(user.token))},
	                                            {"email",AVU().WithValue(AV(user.email))},
	                                            {"phone",AVU().WithValue(AV(user.phone))},
	                                            {"institution",AVU().WithValue(AV(user.institution))},
	                                            {"sshKey",AVU().WithValue(AV(user.sshKey))},
	                                            {"lastUseTime",AVU().WithValue(AV(user.lastUseTime))},
	                                            {"superuser",AVU().WithValue(AV().SetBool(user.superuser))},
	                                            {"serviceAccount",AVU().WithValue(AV().SetBool(user.serviceAccount))}
	                                 }));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to update user record: " << err.GetMessage());
		return false;
	}
	
	//update caches
	CacheRecord<User> record(user,userCacheValidity);
	//userCache.upsert(user.unixName,[&record](CacheRecord<User>& existing){ existing=record; },record);
	replaceCacheRecord(userCache,user.unixName,record);
	//if the token has changed, ensure that any old cache record is removed
	if(oldUser.token!=user.token)
		userByTokenCache.erase(oldUser.token);
	replaceCacheRecord(userByTokenCache,user.token,record);
	replaceCacheRecord(userByGlobusIDCache,user.globusID,record);
	
	return true;
}

bool PersistentStore::removeUser(const std::string& id){
	//erase cache entries
	{
		//Somewhat hacky: we can't erase the secondary cache entries unless we know 
		//the keys. However, we keep the caches synchronized, so if there is 
		//such an entry to delete there is also an entry in the main cache, so 
		//we can grab that to get the name without having to read from the 
		//database.
		CacheRecord<User> record;
		bool cached=userCache.find(id,record);
		if(cached){
			//don't particularly care whether the record is expired; if it is 
			//all that will happen is that we will delete the equally stale 
			//record in the other cache
			userByTokenCache.erase(record.record.token);
			userByGlobusIDCache.erase(record.record.globusID);
			groupMembershipByUserCache.erase(id);
		}
		userCache.erase(id);
	}
	
	using Aws::DynamoDB::Model::AttributeValue;
	auto outcome=dbClient.DeleteItem(Aws::DynamoDB::Model::DeleteItemRequest()
								     .WithTableName(userTableName)
								     .WithKey({{"unixName",AttributeValue(id)},
	                                           {"sortKey",AttributeValue(id)}}));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to delete user record: " << err.GetMessage());
		return false;
	}
	
	//clean up any secondary attribute records tied to the user
	databaseScans++;
	Aws::DynamoDB::Model::ScanRequest request;
	request.SetTableName(userTableName);
	request.SetFilterExpression("attribute_exists(#extra) AND #name = "+id);
	request.SetExpressionAttributeNames({{"#extra", "secondaryAttribute"},{"#name", "unixName"}});
	bool keepGoing=false;
	
	std::vector<std::string> toDelete;
	do{
		auto outcome=dbClient.Scan(request);
		if(!outcome.IsSuccess()){
			//TODO: more principled logging or reporting of the nature of the error
			auto err=outcome.GetError();
			log_error("Failed to fetch user secondary records: " << err.GetMessage());
			return false;
		}
		const auto& result=outcome.GetResult();
		//set up fetching the next page if necessary
		if(!result.GetLastEvaluatedKey().empty()){
			keepGoing=true;
			request.SetExclusiveStartKey(result.GetLastEvaluatedKey());
		}
		else
			keepGoing=false;
		//collect results from this page
		for(const auto& item : result.GetItems())
			toDelete.push_back(findOrThrow(item,"sortKey","user secondary record missing sortKey attribute").GetS());
	}while(keepGoing);
	
	for(const auto& sortKey : toDelete){
		outcome=dbClient.DeleteItem(Aws::DynamoDB::Model::DeleteItemRequest()
										 .WithTableName(userTableName)
										 .WithKey({{"unixName",AttributeValue(id)},
												   {"sortKey",AttributeValue(sortKey)}}));
		if(!outcome.IsSuccess()){
			auto err=outcome.GetError();
			log_error("Failed to delete user record: " << err.GetMessage());
			return false;
		}
	}
	return true;
}

std::vector<User> PersistentStore::listUsers(){
	std::vector<User> collected;
	//First check if users are cached
	if(userCacheExpirationTime.load() > std::chrono::steady_clock::now()){
		auto table = userCache.lock_table();
		for(auto itr = table.cbegin(); itr != table.cend(); itr++){
			auto user = itr->second;
			cacheHits++;
			collected.push_back(user);
		}
		table.unlock();
		return collected;
	}
	
	databaseScans++;
	Aws::DynamoDB::Model::ScanRequest request;
	request.SetTableName(userTableName);
	//Ignore group membership records
	request.SetFilterExpression("attribute_not_exists(#groupName) and attribute_not_exists(#secondAttr) and attribute_not_exists(#nextID)");
	request.SetExpressionAttributeNames({{"#groupName", "groupName"},{"#secondAttr", "secondaryAttribute"},{"#nextID", "next_unixID"}});
	bool keepGoing=false;
	
	do{
		auto outcome=dbClient.Scan(request);
		if(!outcome.IsSuccess()){
			//TODO: more principled logging or reporting of the nature of the error
			auto err=outcome.GetError();
			log_error("Failed to fetch user records: " << err.GetMessage());
			return collected;
		}
		const auto& result=outcome.GetResult();
		//set up fetching the next page if necessary
		if(!result.GetLastEvaluatedKey().empty()){
			keepGoing=true;
			request.SetExclusiveStartKey(result.GetLastEvaluatedKey());
		}
		else
			keepGoing=false;
		//collect results from this page
		for(const auto& item : result.GetItems()){
			User user;
			user.valid=true;
			if(item.count("next_unixID"))
				log_fatal("Dynamo is stupid");
			user.unixName=findOrThrow(item,"unixName","user record missing unixName attribute").GetS();
			//try{
			user.name=findOrThrow(item,"name","user record missing name attribute").GetS();
			/*}catch(std::exception& e){
				std::cout << "user " << user.unixName << " missing name" << std::endl;
				for(const auto& entry : item){
					std::cout << " attribute: " << entry.first << " -> ";
					switch(entry.second.GetType()){
						case Aws::DynamoDB::Model::ValueType::STRING:
							std::cout << "[string]\"" << entry.second.GetS() << "\"\n";
							break;
						case Aws::DynamoDB::Model::ValueType::NUMBER:
							std::cout << "[number]" << entry.second.GetN() << '\n';
							break;
						case Aws::DynamoDB::Model::ValueType::BOOL:
							std::cout << "[bool]" << (entry.second.GetBool()?"true":"false") << '\n';
							break;
						default:
							std::cout << "[something else]\n";
							break;
					}
				}
				continue;
			}*/
			user.email=findOrThrow(item,"email","user record missing email attribute").GetS();
			user.phone=findOrDefault(item,"phone",missingString).GetS();
			user.institution=findOrDefault(item,"institution",missingString).GetS();
			user.token=findOrThrow(item,"token","user record missing token attribute").GetS();
			user.globusID=findOrThrow(item,"globusID","user record missing globusID attribute").GetS();
			user.sshKey=findOrThrow(item,"sshKey","user record missing sshKey attribute").GetS();
			user.joinDate=findOrThrow(item,"joinDate","user record missing joinDate attribute").GetS();
			user.lastUseTime=findOrThrow(item,"lastUseTime","user record missing lastUseTime attribute").GetS();
			user.superuser=findOrThrow(item,"superuser","user record missing superuser attribute").GetBool();
			user.serviceAccount=findOrThrow(item,"serviceAccount","user record missing serviceAccount attribute").GetBool();
			user.unixID=std::stoul(findOrThrow(item,"unixID","user record missing unixID attribute (listUsers)").GetN());
			collected.push_back(user);

			CacheRecord<User> record(user,userCacheValidity);
			replaceCacheRecord(userCache,user.unixName,record);
		}
	}while(keepGoing);
	userCacheExpirationTime=std::chrono::steady_clock::now()+userCacheValidity;
	
	return collected;
}

bool PersistentStore::setUserStatusInGroup(const GroupMembership& membership){
	using Aws::DynamoDB::Model::AttributeValue;
	auto request=Aws::DynamoDB::Model::PutItemRequest()
	  .WithTableName(userTableName)
	  .WithItem({
		{"unixName",AttributeValue(membership.userName)},
		{"sortKey",AttributeValue(membership.userName+":"+membership.groupName)},
		{"groupName",AttributeValue(membership.groupName)},
		{"state",AttributeValue(GroupMembership::to_string(membership.state))},
		{"stateSetBy",AttributeValue(membership.stateSetBy)}
	});
	auto outcome=dbClient.PutItem(request);
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to add user group membership record: " << err.GetMessage());
		return false;
	}
	
	//update cache
	CacheRecord<GroupMembership> record(membership,userCacheValidity);
	replaceCacheRecord(groupMembershipCache,membership.userName+":"+membership.groupName,record);
	groupMembershipByUserCache.insert_or_assign(membership.userName,record);
	groupMembershipByGroupCache.insert_or_assign(membership.groupName,record);
	
	return true;
}

bool PersistentStore::removeUserFromGroup(const std::string& uID, std::string groupName){
	//write non-member status to all caches
	GroupMembership membership;
	membership.valid=true;
	membership.userName=uID;
	membership.groupName=groupName;
	membership.state=GroupMembership::NonMember;
	CacheRecord<GroupMembership> record(membership,userCacheValidity);
	replaceCacheRecord(groupMembershipCache,uID+":"+groupName,record);
	groupMembershipByUserCache.insert_or_assign(uID,record);
	groupMembershipByGroupCache.insert_or_assign(groupName,record);

	{
		CacheRecord<GroupMembership> record;
		bool cached=groupMembershipCache.find(groupName,record);
		if (cached){
			groupMembershipByUserCache.erase(uID,record);
			groupMembershipByGroupCache.erase(groupName,record);
			groupMembershipCache.erase(uID);
		}
	}
	
	using Aws::DynamoDB::Model::AttributeValue;
	auto outcome=dbClient.DeleteItem(Aws::DynamoDB::Model::DeleteItemRequest()
								     .WithTableName(userTableName)
								     .WithKey({{"unixName",AttributeValue(uID)},
	                                           {"sortKey",AttributeValue(uID+":"+membership.groupName)}}));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to delete user Group membership record: " << err.GetMessage());
		return false;
	}
	return true;
}

GroupMembership PersistentStore::userStatusInGroup(const std::string& uID, std::string groupName){
	//first see if we have this cached
	{
		CacheRecord<GroupMembership> record;
		if(groupMembershipCache.find(uID+":"+groupName,record)){
			//we have a cached record; is it still valid?
			if(record){ //it is, just return it
				cacheHits++;
				return record;
			}
		}
	}
	//need to query the database
	databaseQueries++;
	log_info("Querying database for user " << uID << " membership in Group " << groupName);
	using Aws::DynamoDB::Model::AttributeValue;
	auto outcome=dbClient.GetItem(Aws::DynamoDB::Model::GetItemRequest()
								  .WithTableName(userTableName)
								  .WithKey({{"unixName",AttributeValue(uID)},
	                                        {"sortKey",AttributeValue(uID+":"+groupName)}}));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to fetch user Group membership record: " << err.GetMessage());
		return GroupMembership{};
	}
	const auto& item=outcome.GetResult().GetItem();
	GroupMembership membership;
	if(item.empty()){ //no match found, make non-member record
		membership.valid=true;
		membership.userName=uID;
		membership.groupName=groupName;
		membership.state=GroupMembership::NonMember;
	}
	else{
		membership.valid=true;
		membership.userName=uID;
		membership.groupName=groupName;
		membership.state=GroupMembership::from_string(findOrThrow(item,"state","membership record missing state attribute").GetS());
		membership.stateSetBy=findOrThrow(item,"stateSetBy","membership record missing state set by attribute").GetS();
	}
	
	//update cache
	CacheRecord<GroupMembership> record(membership,userCacheValidity);
	replaceCacheRecord(groupMembershipCache,uID+":"+groupName,record);
	groupMembershipByUserCache.insert_or_assign(uID,record);
	groupMembershipByGroupCache.insert_or_assign(groupName,record);
	
	return membership;
}

bool PersistentStore::setUserSecondaryAttribute(const std::string& uID, 
                                const std::string& attributeName, 
                                const std::string& attributeValue){
	if(attributeValue.empty())
		throw std::runtime_error("Attribute value must not be empty because Dynamo");
	using AV=Aws::DynamoDB::Model::AttributeValue;
	auto outcome=dbClient.PutItem(Aws::DynamoDB::Model::PutItemRequest()
	                              .WithTableName(userTableName)
	                              .WithItem({{"unixName",AV(uID)},
	                                         {"sortKey",AV(uID+":attr:"+attributeName)},
	                                         {"secondaryAttribute",AV(attributeValue)}
	                              }));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to add User secondary attribute record: " << err.GetMessage());
		return false;
	}
	
	//update caches
	auto record=std::make_pair(attributeName,CacheRecord<std::string>(attributeValue,userCacheValidity));
	std::map<std::string,CacheRecord<std::string>> m{record};
	userAttributeCache.upsert(uID,[&](std::map<std::string,CacheRecord<std::string>>& attrs){
		auto it=attrs.find(record.first);
		if(it==attrs.end())
			attrs.insert(record);
		else
			it->second=record.second;
	},m);
    
	return true;
}

std::string PersistentStore::getUserSecondaryAttribute(const std::string& uID, const std::string& attributeName){
	//first see if we have this cached
	{
		CacheRecord<std::string> record;
		if(userAttributeCache.find_fn(uID,[&](const std::map<std::string,CacheRecord<std::string>>& attrs){
			auto it=attrs.find(attributeName);
			if(it!=attrs.end())
				record=it->second;
		})){
			//we have a cached record; is it still valid?
			if(record){ //it is, just return it
				cacheHits++;
				return record;
			}
		}
	}
	//need to query the database
	databaseQueries++;
	log_info("Querying database for user secondary record " << uID << ':' << attributeName);
	using AV=Aws::DynamoDB::Model::AttributeValue;
	auto outcome=dbClient.GetItem(Aws::DynamoDB::Model::GetItemRequest()
								  .WithTableName(userTableName)
								  .WithKey({{"unixName",AV(uID)},
	                                        {"sortKey",AV(uID+":attr:"+attributeName)}}));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to user secondary record: " << err.GetMessage());
		return std::string{};
	}
	const auto& item=outcome.GetResult().GetItem();
	if(item.empty()) //no match found
		return std::string{};
	
	std::string result=findOrThrow(item,"secondaryAttribute","user secondary record missing attribute").GetS();
	
	//update cache
	auto record=std::make_pair(uID,CacheRecord<std::string>(result,userCacheValidity));
	std::map<std::string,CacheRecord<std::string>> m{record};
	userAttributeCache.upsert(uID,[&](std::map<std::string,CacheRecord<std::string>>& attrs){
		auto it=attrs.find(record.first);
		if(it==attrs.end())
			attrs.insert(record);
		else
			it->second=record.second;
	},m);
	
	return result;
}

bool PersistentStore::removeUserSecondaryAttribute(const std::string& uID, const std::string& attributeName){
	//remove from cache is present there
	userAttributeCache.erase_fn(uID,[&](std::map<std::string,CacheRecord<std::string>>& attrs){
		attrs.erase(attributeName);
		return attrs.empty(); //if the map for this user is empty, remove it entirely
	});
	
	using AV=Aws::DynamoDB::Model::AttributeValue;
	auto outcome=dbClient.DeleteItem(Aws::DynamoDB::Model::DeleteItemRequest()
								     .WithTableName(userTableName)
								     .WithKey({{"unixName",AV(uID)},
	                                           {"sortKey",AV(uID+":attr:"+attributeName)}}));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to delete secondary user record record: " << err.GetMessage());
		return false;
	}
	return true;
}

bool PersistentStore::unixNameInUse(const std::string& name){
	//TODO: Should this be cached?
	//need to query the database
	databaseQueries++;
	using AV=Aws::DynamoDB::Model::AttributeValue;
	auto outcome=dbClient.Query(Aws::DynamoDB::Model::QueryRequest()
								.WithTableName(userTableName)
								.WithKeyConditionExpression("#unixName = :name_val")
								.WithExpressionAttributeNames({{"#unixName","unixName"}})
								.WithExpressionAttributeValues({{":name_val",AV(name)}})
								);
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_fatal("Failed to look up unix name: " << err.GetMessage());
	}
	const auto& queryResult=outcome.GetResult();
	return (queryResult.GetCount()>0);
}

std::vector<GroupMembership> PersistentStore::getUserGroupMemberships(const std::string& uID){
	//first check if list of memberships is cached
	auto cached = groupMembershipByUserCache.find(uID);
	if (cached.second > std::chrono::steady_clock::now()) {
		auto records = cached.first;
		std::vector<GroupMembership> memberships;
		for (auto record : records) {
			cacheHits++;
			memberships.push_back(record);
		}
		return memberships;
	}

	using Aws::DynamoDB::Model::AttributeValue;
	databaseQueries++;
	log_info("Querying database for user " << uID << " Group memberships");
	auto request=Aws::DynamoDB::Model::QueryRequest()
	.WithTableName(userTableName)
	.WithKeyConditionExpression("#id = :id AND begins_with(#sortKey,:prefix)")
	.WithExpressionAttributeNames({
		{"#id","unixName"},
		{"#sortKey","sortKey"}
	})
	.WithExpressionAttributeValues({
		{":id",AttributeValue(uID)},
		{":prefix",AttributeValue(uID+":")}
	});
	auto outcome=dbClient.Query(request);
	std::vector<GroupMembership> memberships;
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to fetch user's Group membership records: " << err.GetMessage());
		return memberships;
	}
	
	const auto& queryResult=outcome.GetResult();
	for(const auto& item : queryResult.GetItems()){
		if(item.count("groupName")){
			GroupMembership membership;
			membership.userName=uID;
			membership.groupName=findOrThrow(item,"groupName","membership record missing group name attribute").GetS();
			membership.state=GroupMembership::from_string(findOrThrow(item,"state","membership record missing state attribute").GetS());
			membership.stateSetBy=findOrThrow(item,"stateSetBy","membership record missing state set by attribute").GetS();
			membership.valid=true;
			memberships.push_back(membership);
			
			CacheRecord<GroupMembership> record(membership,userCacheValidity);
			replaceCacheRecord(groupMembershipCache,uID+":"+membership.groupName,record);
			groupMembershipByUserCache.insert_or_assign(uID,record);
			groupMembershipByGroupCache.insert_or_assign(membership.groupName,record);
		}
	}
	
	auto expirationTime = std::chrono::steady_clock::now() + userCacheValidity;
	groupMembershipByUserCache.update_expiration(uID, expirationTime);
	
	return memberships;
}

//----

bool PersistentStore::addGroup(Group& group){
	if(group.email.empty())
		throw std::runtime_error("Group email must not be empty because Dynamo");
	if(group.phone.empty())
		throw std::runtime_error("Group phone must not be empty because Dynamo");
	if(group.purpose.empty())
		throw std::runtime_error("Group purpose must not be empty because Dynamo");
	if(group.description.empty())
		throw std::runtime_error("Group description must not be empty because Dynamo");
	
	unsigned int groupID=0;
	if(group.unixID!=0){
		log_info("allocating group with ID " << group.unixID);
		bool reserved=allocateSpecificUnixID(groupTableName, "name", minimumGroupID, maximumGroupID, group.name, group.unixID);
		if(!reserved)
			log_fatal("Existing group ID (" << group.unixID << ") is already in use");
	}
	else
		group.unixID=allocateUnixID(groupTableName, "name", minimumGroupID, maximumGroupID, group.name);
		
	using AV=Aws::DynamoDB::Model::AttributeValue;
	auto outcome=dbClient.PutItem(Aws::DynamoDB::Model::PutItemRequest()
	                              .WithTableName(groupTableName)
	                              .WithItem({{"name",AV(group.name)},
	                                         {"sortKey",AV(group.name)},
	                                         {"displayName",AV(group.displayName)},
	                                         {"email",AV(group.email)},
	                                         {"phone",AV(group.phone)},
	                                         {"purpose",AV(group.purpose)},
	                                         {"description",AV(group.description)},
	                                         {"creationDate",AV(group.creationDate)},
	                                         {"unixID",AV().SetN(std::to_string(group.unixID))}
	                              }));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to add Group record: " << err.GetMessage());
		return false;
	}
	
	//update caches
	CacheRecord<Group> record(group,groupCacheValidity);
	replaceCacheRecord(groupCache,group.name,record);
        
	return true;
}

bool PersistentStore::addGroupRequest(GroupRequest& gr){
	if(gr.email.empty())
		throw std::runtime_error("Group email must not be empty because Dynamo");
	if(gr.phone.empty())
		throw std::runtime_error("Group phone must not be empty because Dynamo");
	if(gr.purpose.empty())
		throw std::runtime_error("Group purpose must not be empty because Dynamo");
	if(gr.description.empty())
		throw std::runtime_error("Group description must not be empty because Dynamo");
	using AV=Aws::DynamoDB::Model::AttributeValue;
	
	gr.unixID=allocateUnixID(groupTableName, "name", minimumGroupID, maximumGroupID, gr.name);
	
	AV secondary;
	secondary.AddMEntry("dummy",std::make_shared<AV>("dummy"));
	for(const auto& entry : gr.secondaryAttributes)
		secondary.AddMEntry(entry.first,std::make_shared<AV>(entry.second));
	
	auto outcome=dbClient.PutItem(Aws::DynamoDB::Model::PutItemRequest()
	                              .WithTableName(groupTableName)
	                              .WithItem({{"name",AV(gr.name)},
	                                         {"sortKey",AV(gr.name)},
	                                         {"displayName",AV(gr.displayName)},
	                                         {"email",AV(gr.email)},
	                                         {"phone",AV(gr.phone)},
	                                         {"purpose",AV(gr.purpose)},
	                                         {"description",AV(gr.description)},
	                                         {"requester",AV(gr.requester)},
	                                         {"unixID",AV().SetN(std::to_string(gr.unixID))},
	                                         {"secondaryAttributes",secondary}
	                              }));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to add Group Request record: " << err.GetMessage());
		return false;
	}
	
	//update caches
	CacheRecord<GroupRequest> record(gr,groupCacheValidity);
	replaceCacheRecord(groupRequestCache,gr.name,record);
        
	return true;
}

bool PersistentStore::removeGroup(const std::string& groupName){
	using Aws::DynamoDB::Model::AttributeValue;
	
	//delete all memberships in the group
	for(auto membership : getMembersOfGroup(groupName)){
		if(!removeUserFromGroup(membership.userName,groupName))
			return false;
	}
	
	//erase cache entries
	{
		groupCache.erase(groupName);
		groupRequestCache.erase(groupName);
		groupMembershipByGroupCache.erase(groupName);
	}
	
	//delete the Group record itself
	auto outcome=dbClient.DeleteItem(Aws::DynamoDB::Model::DeleteItemRequest()
								     .WithTableName(groupTableName)
								     .WithKey({{"name",AttributeValue(groupName)},
	                                           {"sortKey",AttributeValue(groupName)}}));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to delete Group record: " << err.GetMessage());
		return false;
	}
	
	//clean up any secondary attribute records tied to the group
	databaseScans++;
	Aws::DynamoDB::Model::ScanRequest request;
	request.SetTableName(groupTableName);
	request.SetFilterExpression("attribute_exists(#extra) AND #name = :name");
	request.SetExpressionAttributeNames({{"#extra", "secondaryAttribute"},{"#name", "name"}});
	request.SetExpressionAttributeValues({{":name",AttributeValue(groupName)}});
	bool keepGoing=false;
	
	std::vector<std::string> toDelete;
	do{
		auto outcome=dbClient.Scan(request);
		if(!outcome.IsSuccess()){
			//TODO: more principled logging or reporting of the nature of the error
			auto err=outcome.GetError();
			log_error("Failed to fetch Group secondary records: " << err.GetMessage());
			return false;
		}
		const auto& result=outcome.GetResult();
		//set up fetching the next page if necessary
		if(!result.GetLastEvaluatedKey().empty()){
			keepGoing=true;
			request.SetExclusiveStartKey(result.GetLastEvaluatedKey());
		}
		else
			keepGoing=false;
		//collect results from this page
		for(const auto& item : result.GetItems())
			toDelete.push_back(findOrThrow(item,"sortKey","Group secondary record missing sortKey attribute").GetS());
	}while(keepGoing);
	
	for(const auto& sortKey : toDelete){
		outcome=dbClient.DeleteItem(Aws::DynamoDB::Model::DeleteItemRequest()
										 .WithTableName(groupTableName)
										 .WithKey({{"name",AttributeValue(groupName)},
												   {"sortKey",AttributeValue(sortKey)}}));
		if(!outcome.IsSuccess()){
			auto err=outcome.GetError();
			log_error("Failed to delete Group record: " << err.GetMessage());
			return false;
		}
	}
	
	return true;
}

bool PersistentStore::updateGroup(const Group& group){
	using AV=Aws::DynamoDB::Model::AttributeValue;
	using AVU=Aws::DynamoDB::Model::AttributeValueUpdate;
	auto outcome=dbClient.UpdateItem(Aws::DynamoDB::Model::UpdateItemRequest()
	                                 .WithTableName(groupTableName)
	                                 .WithKey({{"name",AV(group.name)},
	                                           {"sortKey",AV(group.name)}})
	                                 .WithAttributeUpdates({
	                                            {"displayName",AVU().WithValue(AV(group.displayName))},
	                                            {"email",AVU().WithValue(AV(group.email))},
	                                            {"phone",AVU().WithValue(AV(group.phone))},
	                                            {"purpose",AVU().WithValue(AV(group.purpose))},
	                                            {"description",AVU().WithValue(AV(group.description))},
	                                            })
	                                 );
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to update Group record: " << err.GetMessage());
		return false;
	}
	
	//update caches
	CacheRecord<Group> record(group,groupCacheValidity);
	replaceCacheRecord(groupCache,group.name,record);
	
	return true;
}

bool PersistentStore::updateGroupRequest(const GroupRequest& request){
	using AV=Aws::DynamoDB::Model::AttributeValue;
	using AVU=Aws::DynamoDB::Model::AttributeValueUpdate;
	
	AV secondary;
	secondary.AddMEntry("dummy",std::make_shared<AV>("dummy"));
	for(const auto& entry : request.secondaryAttributes)
		secondary.AddMEntry(entry.first,std::make_shared<AV>(entry.second));
	
	auto outcome=dbClient.UpdateItem(Aws::DynamoDB::Model::UpdateItemRequest()
	                                 .WithTableName(groupTableName)
	                                 .WithKey({{"name",AV(request.name)},
	                                           {"sortKey",AV(request.name)}})
	                                 .WithAttributeUpdates({
	                                            {"displayName",AVU().WithValue(AV(request.displayName))},
	                                            {"email",AVU().WithValue(AV(request.email))},
	                                            {"phone",AVU().WithValue(AV(request.phone))},
	                                            {"purpose",AVU().WithValue(AV(request.purpose))},
	                                            {"description",AVU().WithValue(AV(request.description))},
	                                            {"requester",AVU().WithValue(AV(request.requester))},
	                                            {"secondaryAttributes",AVU().WithValue(secondary)}
	                                            })
	                                 );
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to update Group Request record: " << err.GetMessage());
		return false;
	}
	
	//update caches
	groupCache.erase(request.name);
	CacheRecord<GroupRequest> record(request,groupCacheValidity);
	replaceCacheRecord(groupRequestCache,request.name,record);
	
	return true;
}

std::vector<GroupMembership> PersistentStore::getMembersOfGroup(const std::string groupName){
	//first check if list of memberships is cached
	CacheRecord<std::string> record;
	auto cached = groupMembershipByGroupCache.find(groupName);
	if (cached.second > std::chrono::steady_clock::now()) {
		auto records = cached.first;
		std::vector<GroupMembership> memberships;
		for (auto record : records) {
			cacheHits++;
			memberships.push_back(record);
		}
		return memberships;
	}

	using Aws::DynamoDB::Model::AttributeValue;
	databaseQueries++;
	log_info("Querying database for members of Group " << groupName);
	auto outcome=dbClient.Query(Aws::DynamoDB::Model::QueryRequest()
	                            .WithTableName(userTableName)
	                            .WithIndexName("ByGroup")
	                            .WithKeyConditionExpression("#groupName = :id_val")
	                            .WithExpressionAttributeNames({{"#groupName","groupName"}})
								.WithExpressionAttributeValues({{":id_val",AttributeValue(groupName)}})
	                            );
	std::vector<GroupMembership> memberships;
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to fetch Group membership records: " << err.GetMessage());
		return memberships;
	}
	const auto& queryResult=outcome.GetResult();
	memberships.reserve(queryResult.GetCount());
	for(const auto& item : queryResult.GetItems()){
		GroupMembership membership;
		membership.userName=findOrThrow(item,"unixName","membership record missing user unixName attribute").GetS();
		membership.groupName=groupName;
		membership.state=GroupMembership::from_string(findOrThrow(item,"state","membership record missing state attribute").GetS());
		membership.stateSetBy=findOrThrow(item,"stateSetBy","membership record missing state set by attribute").GetS();
		membership.valid=true;
		memberships.push_back(membership);
		
		CacheRecord<GroupMembership> record(membership,userCacheValidity);
		replaceCacheRecord(groupMembershipCache,membership.userName+":"+groupName,record);
		groupMembershipByUserCache.insert_or_assign(membership.userName,record);
		groupMembershipByGroupCache.insert_or_assign(groupName,record);
	}
	
	auto expirationTime = std::chrono::steady_clock::now() + groupCacheValidity;
	groupMembershipByGroupCache.update_expiration(groupName, expirationTime);
	
	return memberships;
}

std::vector<Group> PersistentStore::listGroups(){
	//First check if groups are cached
	std::vector<Group> collected;
	if(groupCacheExpirationTime.load() > std::chrono::steady_clock::now()){
	    auto table = groupCache.lock_table();
		for(auto itr = table.cbegin(); itr != table.cend(); itr++){
			cacheHits++;
			collected.push_back(itr->second);
		}
	
		table.unlock();
		return collected;
	}	

	databaseScans++;
	Aws::DynamoDB::Model::ScanRequest request;
	request.SetTableName(groupTableName);
	request.SetFilterExpression("attribute_not_exists(#requester) and attribute_not_exists(#secondAttr) and attribute_not_exists(#nextID)");
	request.SetExpressionAttributeNames({{"#requester", "requester"},{"#secondAttr", "secondaryAttribute"},{"#nextID", "next_unixID"}});
	bool keepGoing=false;
	
	do{
		auto outcome=dbClient.Scan(request);
		if(!outcome.IsSuccess()){
			//TODO: more principled logging or reporting of the nature of the error
			auto err=outcome.GetError();
			log_error("Failed to fetch Group records: " << err.GetMessage());
			return collected;
		}
		const auto& result=outcome.GetResult();
		//set up fetching the next page if necessary
		if(!result.GetLastEvaluatedKey().empty()){
			keepGoing=true;
			request.SetExclusiveStartKey(result.GetLastEvaluatedKey());
		}
		else
			keepGoing=false;
		//collect results from this page
		for(const auto& item : result.GetItems()){
			
		
			Group group;
			group.valid=true;
			group.name=findOrThrow(item,"name","Group record missing name attribute").GetS();
			//try{
			group.displayName=findOrThrow(item,"displayName","Group record missing displayName attribute").GetS();
			/*}catch(std::exception& e){
				std::cout << "Group " << group.name << " missing displayName" << std::endl;
				for(const auto& entry : item){
					std::cout << " attribute: " << entry.first << " -> ";
					switch(entry.second.GetType()){
						case Aws::DynamoDB::Model::ValueType::STRING:
							std::cout << "[string]\"" << entry.second.GetS() << "\"\n";
							break;
						case Aws::DynamoDB::Model::ValueType::NUMBER:
							std::cout << "[number]" << entry.second.GetN() << '\n';
							break;
						case Aws::DynamoDB::Model::ValueType::BOOL:
							std::cout << "[bool]" << (entry.second.GetBool()?"true":"false") << '\n';
							break;
						default:
							std::cout << "[something else]\n";
							break;
					}
				}
				continue;
			}*/
			group.email=findOrThrow(item,"email","Group record missing email attribute").GetS();
			group.phone=findOrThrow(item,"phone","Group record missing phone attribute").GetS();
			group.purpose=findOrThrow(item,"purpose","Group record missing purpose attribute").GetS();
			group.description=findOrThrow(item,"description","Group record missing description attribute").GetS();
			group.creationDate=findOrThrow(item,"creationDate","Group record missing creation date attribute").GetS();
			group.unixID=std::stoul(findOrThrow(item,"unixID","Group record missing unixID attribute").GetN());
			collected.push_back(group);

			CacheRecord<Group> record(group,groupCacheValidity);
			replaceCacheRecord(groupCache,group.name,record);
		}
	}while(keepGoing);
	groupCacheExpirationTime=std::chrono::steady_clock::now()+groupCacheValidity;
	
	return collected;
}

std::vector<GroupRequest> PersistentStore::listGroupRequests(){
	//First check if group requests are cached
	std::vector<GroupRequest> collected;
	if(groupRequestCacheExpirationTime.load() > std::chrono::steady_clock::now()){
	    auto table = groupRequestCache.lock_table();
		for(auto itr = table.cbegin(); itr != table.cend(); itr++){
			cacheHits++;
			collected.push_back(itr->second);
		}
	
		table.unlock();
		return collected;
	}	

	databaseScans++;
	Aws::DynamoDB::Model::ScanRequest request;
	request.SetTableName(groupTableName);
	request.SetFilterExpression("attribute_exists(#requester) and attribute_not_exists(#secondAttr)");
	request.SetExpressionAttributeNames({{"#requester", "requester"},{"#secondAttr", "secondaryAttribute"}});
	bool keepGoing=false;
	
	do{
		auto outcome=dbClient.Scan(request);
		if(!outcome.IsSuccess()){
			//TODO: more principled logging or reporting of the nature of the error
			auto err=outcome.GetError();
			log_error("Failed to fetch Group records: " << err.GetMessage());
			return collected;
		}
		const auto& result=outcome.GetResult();
		//set up fetching the next page if necessary
		if(!result.GetLastEvaluatedKey().empty()){
			keepGoing=true;
			request.SetExclusiveStartKey(result.GetLastEvaluatedKey());
		}
		else
			keepGoing=false;
		//collect results from this page
		for(const auto& item : result.GetItems()){
			GroupRequest gr;
			gr.valid=true;
			gr.name=findOrThrow(item,"name","Group request record missing name attribute").GetS();
			gr.displayName=findOrThrow(item,"displayName","Group request record missing displayName attribute").GetS();
			gr.email=findOrThrow(item,"email","Group request record missing email attribute").GetS();
			gr.phone=findOrThrow(item,"phone","Group request record missing phone attribute").GetS();
			gr.purpose=findOrThrow(item,"purpose","Group request record missing purpose attribute").GetS();
			gr.description=findOrThrow(item,"description","Group request record missing description attribute").GetS();
			gr.requester=findOrThrow(item,"requester","Group request record missing requester attribute").GetS();
			gr.unixID=std::stoul(findOrThrow(item,"unixID","Group request record missing unixID attribute").GetN());
			auto extra=findOrThrow(item,"secondaryAttributes","Group Request record missing secondary attributes").GetM();
			for(const auto& attr : extra){
				if(attr.first=="dummy")
					continue;
				gr.secondaryAttributes[attr.first]=attr.second->GetS();
			}
			collected.push_back(gr);

			CacheRecord<GroupRequest> record(gr,groupCacheValidity);
			replaceCacheRecord(groupRequestCache,gr.name,record);
		}
	}while(keepGoing);
	groupRequestCacheExpirationTime=std::chrono::steady_clock::now()+groupCacheValidity;
	
	return collected;
}

std::vector<GroupRequest> PersistentStore::listGroupRequestsByRequester(const std::string& requester){
	//TODO: add caching for these queries?

	using Aws::DynamoDB::Model::AttributeValue;
	databaseQueries++;
	log_info("Querying database for group requests by " << requester);
	auto outcome=dbClient.Query(Aws::DynamoDB::Model::QueryRequest()
	                            .WithTableName(groupTableName)
	                            .WithIndexName("ByRequester")
	                            .WithKeyConditionExpression("#requester = :id_val")
	                            .WithExpressionAttributeNames({{"#requester","requester"}})
								.WithExpressionAttributeValues({{":id_val",AttributeValue(requester)}})
	                            );
	std::vector<GroupRequest> requests;
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to fetch Group Request records: " << err.GetMessage());
		return requests;
	}
	const auto& queryResult=outcome.GetResult();
	requests.reserve(queryResult.GetCount());
	for(const auto& item : queryResult.GetItems()){
		std::string name=findOrThrow(item,"name","Group request record missing name attribute").GetS();
		GroupRequest gr=getGroupRequest(name);
		if(gr)
			requests.push_back(gr);

		CacheRecord<GroupRequest> record(gr,groupCacheValidity);
		replaceCacheRecord(groupRequestCache,gr.name,record);
	}
	
	return requests;
}

Group PersistentStore::getGroup(const std::string& groupName){
	//first see if we have this cached
	{
		CacheRecord<Group> record;
		if(groupCache.find(groupName,record)){
			//we have a cached record; is it still valid?
			if(record){ //it is, just return it
				cacheHits++;
				return record;
			}
		}
	}
	//need to query the database
	databaseQueries++;
	log_info("Querying database for Group " << groupName);
	using Aws::DynamoDB::Model::AttributeValue;
	auto outcome=dbClient.GetItem(Aws::DynamoDB::Model::GetItemRequest()
	                              .WithTableName(groupTableName)
	                              .WithKey({{"name",AttributeValue(groupName)},
	                                        {"sortKey",AttributeValue(groupName)}}));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to fetch Group record: " << err.GetMessage());
		return Group();
	}
	const auto& item=outcome.GetResult().GetItem();
	if(item.empty()) //no match found
		return Group{};
	Group group;
	group.valid=true;
	group.name=groupName;
	group.displayName=findOrThrow(item,"displayName","Group record missing displayName attribute").GetS();
	group.email=findOrThrow(item,"email","Group record missing email attribute").GetS();
	group.phone=findOrThrow(item,"phone","Group record missing phone attribute").GetS();
	group.purpose=findOrThrow(item,"purpose","Group record missing purpose attribute").GetS();
	group.description=findOrThrow(item,"description","Group record missing description attribute").GetS();
	if(item.count("requester")) //if a requestor is recorded this group is still in a requested state
		group.pending=true;
	else{ //only fully created groups have these properties
		group.creationDate=findOrThrow(item,"creationDate","Group record missing creation date attribute").GetS();
	}
	group.unixID=std::stoul(findOrThrow(item,"unixID","Group record missing unixID attribute").GetN());
	
	//update caches
	CacheRecord<Group> record(group,groupCacheValidity);
	replaceCacheRecord(groupCache,groupName,record);
	
	return group;
}

GroupRequest PersistentStore::getGroupRequest(const std::string& groupName){
	//first see if we have this cached
	{
		CacheRecord<GroupRequest> record;
		if(groupRequestCache.find(groupName,record)){
			//we have a cached record; is it still valid?
			if(record){ //it is, just return it
				cacheHits++;
				return record;
			}
		}
	}
	//need to query the database
	databaseQueries++;
	log_info("Querying database for Group " << groupName);
	using Aws::DynamoDB::Model::AttributeValue;
	auto outcome=dbClient.GetItem(Aws::DynamoDB::Model::GetItemRequest()
	                              .WithTableName(groupTableName)
	                              .WithKey({{"name",AttributeValue(groupName)},
	                                        {"sortKey",AttributeValue(groupName)}}));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to fetch Group Request record: " << err.GetMessage());
		return GroupRequest();
	}
	const auto& item=outcome.GetResult().GetItem();
	if(item.empty() || !item.count("requester")) //no match found
		return GroupRequest{};
	GroupRequest gr;
	gr.valid=true;
	gr.name=groupName;
	gr.displayName=findOrThrow(item,"displayName","Group Request record missing displayName attribute").GetS();
	gr.email=findOrThrow(item,"email","Group Request record missing email attribute").GetS();
	gr.phone=findOrThrow(item,"phone","Group Request record missing phone attribute").GetS();
	gr.purpose=findOrThrow(item,"purpose","Group Request record missing purpose attribute").GetS();
	gr.description=findOrThrow(item,"description","Group Request record missing description attribute").GetS();
	gr.requester=findOrThrow(item,"requester","Group Request record missing requester attribute").GetS();
	gr.unixID=std::stoul(findOrThrow(item,"unixID","Group Request record missing unixID attribute").GetN());
	
	auto extra=findOrThrow(item,"secondaryAttributes","Group Request record missing secondary attributes").GetM();
	for(const auto& attr : extra){
		if(attr.first=="dummy")
			continue;
		gr.secondaryAttributes[attr.first]=attr.second->GetS();
	}
	
	//update caches
	CacheRecord<GroupRequest> record(gr,groupCacheValidity);
	replaceCacheRecord(groupRequestCache,groupName,record);
	
	return gr;
}

bool PersistentStore::approveGroupRequest(const std::string& groupName){
	//get secondary attributes to put them into their own records
	const GroupRequest gr=getGroupRequest(groupName);
	if(!gr.valid){
		log_error("Group request " << groupName << " could not be fetched");
		return false;
	}
	
	std::string creationDate=timestamp();
	using AV=Aws::DynamoDB::Model::AttributeValue;
	using AVU=Aws::DynamoDB::Model::AttributeValueUpdate;
	auto outcome=dbClient.UpdateItem(Aws::DynamoDB::Model::UpdateItemRequest()
	                                 .WithTableName(groupTableName)
	                                 .WithKey({{"name",AV(groupName)},
	                                           {"sortKey",AV(groupName)}})
	                                 .WithAttributeUpdates({
	                                            {"requester",AVU().WithAction(Aws::DynamoDB::Model::AttributeAction::DELETE_)},
	                                            {"secondaryAttributes",AVU().WithAction(Aws::DynamoDB::Model::AttributeAction::DELETE_)},
	                                            {"creationDate",AVU().WithValue(AV(creationDate))},
	                                            })
	                                 );
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to convert Group Request record into Group Record: " << err.GetMessage());
		return false;
	}
	
	for(const auto& attr : gr.secondaryAttributes){
		if(attr.first=="dummy")
			continue;
		setGroupSecondaryAttribute(gr.name,attr.first,attr.second);
	}
	
	{ //make sure no old, incorrect cache entries persist
		groupCache.erase(groupName);
		groupRequestCache.erase(groupName);
		groupMembershipByGroupCache.erase(groupName);
	}
	{ //update the group cache
		CacheRecord<Group> record(Group(gr,creationDate),groupCacheValidity);
		record.record.pending=false; //explicitly mark as no longer pending
		replaceCacheRecord(groupCache,gr.name,record);
	}
	
	return true;
}

bool PersistentStore::setGroupSecondaryAttribute(const std::string& groupName, 
                                const std::string& attributeName, 
                                const std::string& attributeValue){
	if(attributeValue.empty())
		throw std::runtime_error("Attribute value must not be empty because Dynamo");
	using AV=Aws::DynamoDB::Model::AttributeValue;
	auto outcome=dbClient.PutItem(Aws::DynamoDB::Model::PutItemRequest()
	                              .WithTableName(groupTableName)
	                              .WithItem({{"name",AV(groupName)},
	                                         {"sortKey",AV(groupName+":attr:"+attributeName)},
	                                         {"secondaryAttribute",AV(attributeValue)}
	                              }));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to add Group secondary attribute record: " << err.GetMessage());
		return false;
	}
	
	//update caches
	auto record=std::make_pair(attributeName,CacheRecord<std::string>(attributeValue,groupCacheValidity));
	std::map<std::string,CacheRecord<std::string>> m{record};
	groupAttributeCache.upsert(groupName,[&](std::map<std::string,CacheRecord<std::string>>& attrs){
		auto it=attrs.find(record.first);
		if(it==attrs.end())
			attrs.insert(record);
		else
			it->second=record.second;
	},m);
    
	return true;
}

std::string PersistentStore::getGroupSecondaryAttribute(const std::string& groupName, const std::string& attributeName){
	//first see if we have this cached
	{
		CacheRecord<std::string> record;
		if(groupAttributeCache.find_fn(groupName,[&](const std::map<std::string,CacheRecord<std::string>>& attrs){
			auto it=attrs.find(attributeName);
			if(it!=attrs.end())
				record=it->second;
		})){
			//we have a cached record; is it still valid?
			if(record){ //it is, just return it
				cacheHits++;
				return record;
			}
		}
	}
	//need to query the database
	databaseQueries++;
	log_info("Querying database for group secondary record " << groupName << ':' << attributeName);
	using AV=Aws::DynamoDB::Model::AttributeValue;
	auto outcome=dbClient.GetItem(Aws::DynamoDB::Model::GetItemRequest()
								  .WithTableName(groupTableName)
								  .WithKey({{"name",AV(groupName)},
	                                        {"sortKey",AV(groupName+":attr:"+attributeName)}}));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to group secondary record: " << err.GetMessage());
		return std::string{};
	}
	const auto& item=outcome.GetResult().GetItem();
	if(item.empty()) //no match found
		return std::string{};
	
	std::string result=findOrThrow(item,"secondaryAttribute","group secondary record missing attribute").GetS();
	
	//update cache
	auto record=std::make_pair(groupName,CacheRecord<std::string>(result,groupCacheValidity));
	std::map<std::string,CacheRecord<std::string>> m{record};
	groupAttributeCache.upsert(groupName,[&](std::map<std::string,CacheRecord<std::string>>& attrs){
		auto it=attrs.find(record.first);
		if(it==attrs.end())
			attrs.insert(record);
		else
			it->second=record.second;
	},m);
	
	return result;
}

bool PersistentStore::removeGroupSecondaryAttribute(const std::string& groupName, const std::string& attributeName){
	//remove from cache is present there
	groupAttributeCache.erase_fn(groupName,[&](std::map<std::string,CacheRecord<std::string>>& attrs){
		attrs.erase(attributeName);
		return attrs.empty(); //if the map for this group is empty, remove it entirely
	});
	
	using AV=Aws::DynamoDB::Model::AttributeValue;
	auto outcome=dbClient.DeleteItem(Aws::DynamoDB::Model::DeleteItemRequest()
								     .WithTableName(groupTableName)
								     .WithKey({{"name",AV(groupName)},
	                                           {"sortKey",AV(groupName+":attr:"+attributeName)}}));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to delete secondary group record record: " << err.GetMessage());
		return false;
	}
	return true;
}

std::string PersistentStore::getStatistics() const{
	std::ostringstream os;
	os << "Cache hits: " << cacheHits.load() << "\n";
	os << "Database queries: " << databaseQueries.load() << "\n";
	os << "Database scans: " << databaseScans.load() << "\n";
	return os.str();
}

const User authenticateUser(PersistentStore& store, const char* token){
	if(token==nullptr) //no token => no way of identifying a valid user
		return User{};
	return store.findUserByToken(token);
}
