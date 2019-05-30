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

#include <aws/dynamodb/model/CreateGlobalSecondaryIndexAction.h>
#include <aws/dynamodb/model/CreateTableRequest.h>
#include <aws/dynamodb/model/DeleteTableRequest.h>
#include <aws/dynamodb/model/DescribeTableRequest.h>
#include <aws/dynamodb/model/UpdateTableRequest.h>

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
	
} //anonymous namespace

PersistentStore::PersistentStore(const Aws::Auth::AWSCredentials& credentials, 
                                 const Aws::Client::ClientConfiguration& clientConfig,
                                 std::string bootstrapUserFile):
	dbClient(credentials,clientConfig),
	userTableName("CONNECT_users"),
	groupTableName("CONNECT_groups"),
	userCacheValidity(std::chrono::minutes(5)),
	userCacheExpirationTime(std::chrono::steady_clock::now()),
	groupCacheValidity(std::chrono::minutes(30)),
	groupCacheExpirationTime(std::chrono::steady_clock::now()),
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
		                       .WithNonKeyAttributes({"ID","name","email","phone","institution","globusID","sshKey","superuser","serviceAccount"}))
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
		                       .WithNonKeyAttributes({"ID","token"}))
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
		                       .WithNonKeyAttributes({"ID","state","stateSetBy"}))
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
			AttDef().WithAttributeName("ID").WithAttributeType(SAT::S),
			AttDef().WithAttributeName("sortKey").WithAttributeType(SAT::S),
			AttDef().WithAttributeName("token").WithAttributeType(SAT::S),
			AttDef().WithAttributeName("globusID").WithAttributeType(SAT::S),
			AttDef().WithAttributeName("groupName").WithAttributeType(SAT::S)
		});
		request.SetKeySchema({
			KeySchemaElement().WithAttributeName("ID").WithKeyType(KeyType::HASH),
			KeySchemaElement().WithAttributeName("sortKey").WithKeyType(KeyType::RANGE)
		});
		request.SetProvisionedThroughput(ProvisionedThroughput()
		                                 .WithReadCapacityUnits(1)
		                                 .WithWriteCapacityUnits(1));
		request.AddGlobalSecondaryIndexes(getByTokenIndex());
		request.AddGlobalSecondaryIndexes(getByGlobusIDIndex());
		request.AddGlobalSecondaryIndexes(getByGroupIndex());
		
		auto createOut=dbClient.CreateTable(request);
		if(!createOut.IsSuccess())
			log_fatal("Failed to create user table: " + createOut.GetError().GetMessage());
		
		waitTableReadiness(dbClient,userTableName);
		
		{
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
	}
}

void PersistentStore::InitializeGroupTable(){
	using namespace Aws::DynamoDB::Model;
	using AttDef=Aws::DynamoDB::Model::AttributeDefinition;
	using SAT=Aws::DynamoDB::Model::ScalarAttributeType;
	
	//define indices
	
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
		});
		request.SetKeySchema({
			KeySchemaElement().WithAttributeName("name").WithKeyType(KeyType::HASH),
			KeySchemaElement().WithAttributeName("sortKey").WithKeyType(KeyType::RANGE)
		});
		request.SetProvisionedThroughput(ProvisionedThroughput()
		                                 .WithReadCapacityUnits(1)
		                                 .WithWriteCapacityUnits(1));
		//request.AddGlobalSecondaryIndexes(getByNameIndex());
		
		auto createOut=dbClient.CreateTable(request);
		if(!createOut.IsSuccess())
			log_fatal("Failed to create groups table: " + createOut.GetError().GetMessage());
		
		waitTableReadiness(dbClient,groupTableName);
		
		{
			try{
				Group rootGroup;
				rootGroup.name="root";
				rootGroup.email="none";
				rootGroup.phone="none";
				rootGroup.scienceField="ResourceProvider";
				rootGroup.description="Root group which contains all users but is associated with no resources";
				rootGroup.valid=true;
				if(!addGroup(rootGroup))
					log_fatal("Failed to inject root group");
				
				GroupMembership rootOwnership;
				rootOwnership.userID=rootUser.id;
				rootOwnership.groupName=rootGroup.name;
				rootOwnership.state=GroupMembership::Admin;
				rootOwnership.stateSetBy=rootUser.id;
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
	}
}

void PersistentStore::InitializeTables(std::string bootstrapUserFile){
	{
		std::ifstream credFile(bootstrapUserFile);
		if(!credFile)
			log_fatal("Unable to read root user credentials");
		credFile >> rootUser.id >> rootUser.name >> rootUser.email 
				 >> rootUser.phone;
		credFile.ignore(1024,'\n');
		std::getline(credFile,rootUser.institution);
		credFile >> rootUser.token;
		if(credFile.fail())
			log_fatal("Unable to read root user credentials");
		rootUser.globusID="No Globus ID";
		rootUser.sshKey="No SSH key";
		rootUser.superuser=true;
		rootUser.serviceAccount=true;
		rootUser.valid=true;
	}
	InitializeUserTable();
	InitializeGroupTable();
}

bool PersistentStore::addUser(const User& user){
	using Aws::DynamoDB::Model::AttributeValue;
	auto request=Aws::DynamoDB::Model::PutItemRequest()
	.WithTableName(userTableName)
	.WithItem({
		{"ID",AttributeValue(user.id)},
		{"sortKey",AttributeValue(user.id)},
		{"name",AttributeValue(user.name)},
		{"globusID",AttributeValue(user.globusID)},
		{"token",AttributeValue(user.token)},
		{"email",AttributeValue(user.email)},
		{"phone",AttributeValue(user.phone)},
		{"institution",AttributeValue(user.institution)},
		{"sshKey",AttributeValue(user.sshKey)},
		{"superuser",AttributeValue().SetBool(user.superuser)},
		{"serviceAccount",AttributeValue().SetBool(user.serviceAccount)}
	});
	auto outcome=dbClient.PutItem(request);
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to add user record: " << err.GetMessage());
		return false;
	}
	
	//update caches
	CacheRecord<User> record(user,userCacheValidity);
	userCache.insert_or_assign(user.id,record);
	userByTokenCache.insert_or_assign(user.token,record);
	userByGlobusIDCache.insert_or_assign(user.globusID,record);
	
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
								  .WithKey({{"ID",AttributeValue(id)},
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
	user.id=id;
	user.name=findOrThrow(item,"name","user record missing name attribute").GetS();
	user.email=findOrThrow(item,"email","user record missing email attribute").GetS();
	user.phone=findOrDefault(item,"phone",missingString).GetS();
	user.institution=findOrDefault(item,"institution",missingString).GetS();
	user.token=findOrThrow(item,"token","user record missing token attribute").GetS();
	user.globusID=findOrThrow(item,"globusID","user record missing globusID attribute").GetS();
	user.sshKey=findOrThrow(item,"sshKey","user record missing sshKey attribute").GetS();
	user.superuser=findOrThrow(item,"superuser","user record missing superuser attribute").GetBool();
	user.serviceAccount=findOrThrow(item,"serviceAccount","user record missing serviceAccount attribute").GetBool();
	
	//update caches
	CacheRecord<User> record(user,userCacheValidity);
	userCache.insert_or_assign(user.id,record);
	userByTokenCache.insert_or_assign(user.token,record);
	userByGlobusIDCache.insert_or_assign(user.globusID,record);
	
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
	User user;
	user.valid=true;
	user.token=token;
	user.id=findOrThrow(item,"ID","user record missing ID attribute").GetS();
	user.name=findOrThrow(item,"name","user record missing name attribute").GetS();
	user.email=findOrThrow(item,"email","user record missing email attribute").GetS();
	user.phone=findOrDefault(item,"phone",missingString).GetS();
	user.institution=findOrDefault(item,"institution",missingString).GetS();
	user.globusID=findOrThrow(item,"globusID","user record missing globusID attribute").GetS();
	user.sshKey=findOrThrow(item,"sshKey","user record missing sshKey attribute").GetS();
	user.superuser=findOrThrow(item,"superuser","user record missing superuser attribute").GetBool();
	user.serviceAccount=findOrThrow(item,"serviceAccount","user record missing serviceAccount attribute").GetBool();
	
	//update caches
	CacheRecord<User> record(user,userCacheValidity);
	userCache.insert_or_assign(user.id,record);
	userByTokenCache.insert_or_assign(user.token,record);
	userByGlobusIDCache.insert_or_assign(user.globusID,record);
	
	return user;
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
	user.id=findOrThrow(item,"ID","user record missing ID attribute").GetS();
	user.token=findOrThrow(item,"token","user record missing token attribute").GetS();
	user.globusID=globusID;
	
	//update caches
	CacheRecord<User> record(user,userCacheValidity);
	//We don't have enough information to populate the other caches. :(
	//userCache.insert_or_assign(user.id,record);
	//userByTokenCache.insert_or_assign(user.token,record);
	userByGlobusIDCache.insert_or_assign(user.globusID,record);
	
	return user;
}

bool PersistentStore::updateUser(const User& user, const User& oldUser){
	using AV=Aws::DynamoDB::Model::AttributeValue;
	using AVU=Aws::DynamoDB::Model::AttributeValueUpdate;
	auto outcome=dbClient.UpdateItem(Aws::DynamoDB::Model::UpdateItemRequest()
	                                 .WithTableName(userTableName)
									 .WithKey({{"ID",AV(user.id)},
	                                           {"sortKey",AV(user.id)}})
	                                 .WithAttributeUpdates({
	                                            {"name",AVU().WithValue(AV(user.name))},
	                                            {"globusID",AVU().WithValue(AV(user.globusID))},
	                                            {"token",AVU().WithValue(AV(user.token))},
	                                            {"email",AVU().WithValue(AV(user.email))},
	                                            {"phone",AVU().WithValue(AV(user.phone))},
	                                            {"institution",AVU().WithValue(AV(user.institution))},
	                                            {"sshKey",AVU().WithValue(AV(user.sshKey))},
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
	userCache.insert_or_assign(user.id,record);
	//if the token has changed, ensure that any old cache record is removed
	if(oldUser.token!=user.token)
		userByTokenCache.erase(oldUser.token);
	userByTokenCache.insert_or_assign(user.token,record);
	userByGlobusIDCache.insert_or_assign(user.globusID,record);
	
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
								     .WithKey({{"ID",AttributeValue(id)},
	                                           {"sortKey",AttributeValue(id)}}));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to delete user record: " << err.GetMessage());
		return false;
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
	//request.SetAttributesToGet({"ID","name","email"});
	request.SetFilterExpression("attribute_not_exists(#groupID)");
	request.SetExpressionAttributeNames({{"#groupID", "groupID"}});
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
			user.id=findOrThrow(item,"ID","user record missing ID attribute").GetS();
			user.name=findOrThrow(item,"name","user record missing name attribute").GetS();
			user.email=findOrThrow(item,"email","user record missing email attribute").GetS();
			user.phone=findOrDefault(item,"phone",missingString).GetS();
			user.institution=findOrDefault(item,"institution",missingString).GetS();
			user.token=findOrThrow(item,"token","user record missing token attribute").GetS();
			user.globusID=findOrThrow(item,"globusID","user record missing globusID attribute").GetS();
			user.sshKey=findOrThrow(item,"sshKey","user record missing sshKey attribute").GetS();
			user.superuser=findOrThrow(item,"superuser","user record missing superuser attribute").GetBool();
			user.serviceAccount=findOrThrow(item,"serviceAccount","user record missing serviceAccount attribute").GetBool();
			collected.push_back(user);

			CacheRecord<User> record(user,userCacheValidity);
			userCache.insert_or_assign(user.id,record);
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
		{"ID",AttributeValue(membership.userID)},
		{"sortKey",AttributeValue(membership.userID+":"+membership.groupName)},
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
	groupMembershipCache.insert_or_assign(membership.userID+":"+membership.groupName,record);
	groupMembershipByUserCache.insert_or_assign(membership.userID,record);
	groupMembershipByGroupCache.insert_or_assign(membership.groupName,record);
	
	return true;
}

bool PersistentStore::removeUserFromGroup(const std::string& uID, std::string groupName){
	//write non-member status to all caches
	GroupMembership membership;
	membership.valid=false;
	membership.userID=uID;
	membership.groupName=groupName;
	membership.state=GroupMembership::NonMember;
	CacheRecord<GroupMembership> record(membership,userCacheValidity);
	groupMembershipCache.insert_or_assign(uID+":"+groupName,record);
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
								     .WithKey({{"ID",AttributeValue(uID)},
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
								  .WithKey({{"ID",AttributeValue(uID)},
	                                        {"sortKey",AttributeValue(uID+":"+groupName)}}));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to fetch user Group membership record: " << err.GetMessage());
		return GroupMembership{};
	}
	const auto& item=outcome.GetResult().GetItem();
	GroupMembership membership;
	if(item.empty()){ //no match found, make non-member record
		membership.valid=false;
		membership.userID=uID;
		membership.groupName=groupName;
		membership.state=GroupMembership::NonMember;
	}
	else{
		membership.valid=true;
		membership.userID=uID;
		membership.groupName=groupName;
		membership.state=GroupMembership::from_string(findOrThrow(item,"state","membership record missing state attribute").GetS());
		membership.stateSetBy=findOrThrow(item,"stateSetBy","membership record missing state set by attribute").GetS();
	}
	
	//update cache
	CacheRecord<GroupMembership> record(membership,userCacheValidity);
	groupMembershipCache.insert_or_assign(uID+":"+groupName,record);
	groupMembershipByUserCache.insert_or_assign(uID,record);
	groupMembershipByGroupCache.insert_or_assign(groupName,record);
	
	return membership;
}

std::vector<GroupMembership> PersistentStore::getUserGroupMemberships(const std::string& uID){
	//first check if list of memberships is cached
	CacheRecord<std::string> record;
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
		{"#id","ID"},
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
			membership.userID=uID;
			membership.groupName=findOrThrow(item,"groupName","membership record missing group name attribute").GetS();
			membership.state=GroupMembership::from_string(findOrThrow(item,"state","membership record missing state attribute").GetS());
			membership.stateSetBy=findOrThrow(item,"stateSetBy","membership record missing state set by attribute").GetS();
			membership.valid=true;
			memberships.push_back(membership);
			
			CacheRecord<GroupMembership> record(membership,userCacheValidity);
			groupMembershipCache.insert_or_assign(uID+":"+membership.groupName,record);
			groupMembershipByUserCache.insert_or_assign(uID,record);
			groupMembershipByGroupCache.insert_or_assign(membership.groupName,record);
		}
	}
	
	return memberships;
}

//----

bool PersistentStore::addGroup(const Group& group){
	if(group.email.empty())
		throw std::runtime_error("Group email must not be empty because Dynamo");
	if(group.phone.empty())
		throw std::runtime_error("Group phone must not be empty because Dynamo");
	if(group.scienceField.empty())
		throw std::runtime_error("Group scienceField must not be empty because Dynamo");
	if(group.description.empty())
		throw std::runtime_error("Group description must not be empty because Dynamo");
	using AV=Aws::DynamoDB::Model::AttributeValue;
	auto outcome=dbClient.PutItem(Aws::DynamoDB::Model::PutItemRequest()
	                              .WithTableName(groupTableName)
	                              .WithItem({{"name",AV(group.name)},
	                                         {"sortKey",AV(group.name)},
	                                         {"email",AV(group.email)},
	                                         {"phone",AV(group.phone)},
	                                         {"scienceField",AV(group.scienceField)},
	                                         {"description",AV(group.description)}
	                              }));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to add Group record: " << err.GetMessage());
		return false;
	}
	
	//update caches
	CacheRecord<Group> record(group,groupCacheValidity);
	groupCache.insert_or_assign(group.name,record);
        
	return true;
}

bool PersistentStore::removeGroup(const std::string& groupName){
	using Aws::DynamoDB::Model::AttributeValue;
	
	//delete all memberships in the group
	for(auto membership : getMembersOfGroup(groupName)){
		if(!removeUserFromGroup(membership.userID,groupName))
			return false;
	}
	
	//erase cache entries
	{
		groupCache.erase(groupName);
		groupMembershipByGroupCache.erase(groupName);
	}
	
	//delete the Group record itself
	auto outcome=dbClient.DeleteItem(Aws::DynamoDB::Model::DeleteItemRequest()
								     .WithTableName(groupTableName)
								     .WithKey({{"ID",AttributeValue(groupName)},
	                                           {"sortKey",AttributeValue(groupName)}}));
	if(!outcome.IsSuccess()){
		auto err=outcome.GetError();
		log_error("Failed to delete Group record: " << err.GetMessage());
		return false;
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
	                                            {"email",AVU().WithValue(AV(group.email))},
	                                            {"phone",AVU().WithValue(AV(group.phone))},
	                                            {"scienceField",AVU().WithValue(AV(group.scienceField))},
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
	groupCache.insert_or_assign(group.name,record);
	
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
		membership.userID=findOrThrow(item,"ID","membership record missing user ID attribute").GetS();
		membership.groupName=groupName;
		membership.state=GroupMembership::from_string(findOrThrow(item,"state","membership record missing state attribute").GetS());
		membership.stateSetBy=findOrThrow(item,"stateSetBy","membership record missing state set by attribute").GetS();
		membership.valid=true;
		memberships.push_back(membership);
		
		CacheRecord<GroupMembership> record(membership,userCacheValidity);
		groupMembershipCache.insert_or_assign(membership.userID+":"+groupName,record);
		groupMembershipByUserCache.insert_or_assign(membership.userID,record);
		groupMembershipByGroupCache.insert_or_assign(groupName,record);
	}
	
	return memberships;
}

std::vector<Group> PersistentStore::listGroups(){
	//First check if groups are cached
	std::vector<Group> collected;
	if(groupCacheExpirationTime.load() > std::chrono::steady_clock::now()){
	    auto table = groupCache.lock_table();
		for(auto itr = table.cbegin(); itr != table.cend(); itr++){
		        auto group = itr->second;
			cacheHits++;
			collected.push_back(group);
		}
	
		table.unlock();
		return collected;
	}	

	databaseScans++;
	Aws::DynamoDB::Model::ScanRequest request;
	request.SetTableName(groupTableName);
	request.SetFilterExpression("attribute_exists(#name)");
	request.SetExpressionAttributeNames({{"#name","name"}});
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
			group.email=findOrThrow(item,"email","Group record missing email attribute").GetS();
			group.phone=findOrThrow(item,"phone","Group record missing phone attribute").GetS();
			group.scienceField=findOrThrow(item,"scienceField","Group record missing field of science attribute").GetS();
			group.description=findOrThrow(item,"description","Group record missing description attribute").GetS();
			collected.push_back(group);

			CacheRecord<Group> record(group,groupCacheValidity);
			groupCache.insert_or_assign(group.name,record);
		}
	}while(keepGoing);
	groupCacheExpirationTime=std::chrono::steady_clock::now()+groupCacheValidity;
	
	return collected;
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
	group.email=findOrThrow(item,"email","Group record missing email attribute").GetS();
	group.phone=findOrThrow(item,"phone","Group record missing phone attribute").GetS();
	group.scienceField=findOrThrow(item,"scienceField","Group record missing field of science attribute").GetS();
	group.description=findOrThrow(item,"description","Group record missing description attribute").GetS();
	
	//update caches
	CacheRecord<Group> record(group,groupCacheValidity);
	groupCache.insert_or_assign(groupName,record);
	
	return group;
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
