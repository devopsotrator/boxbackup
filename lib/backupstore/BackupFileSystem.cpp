// --------------------------------------------------------------------------
//
// File
//		Name:    BackupFileSystem.cpp
//		Purpose: Generic interface for reading and writing files and
//			 directories, abstracting over RaidFile, S3, FTP etc.
//		Created: 2015/08/03
//
// --------------------------------------------------------------------------

#include "Box.h"

#include <sys/types.h>

#include <sstream>

#include "autogen_BackupStoreException.h"
#include "BackupStoreDirectory.h"
#include "BackupFileSystem.h"
#include "BackupStoreInfo.h"
#include "CollectInBufferStream.h"
#include "Configuration.h"
#include "S3Client.h"
#include "StoreStructure.h"

#include "MemLeakFindOn.h"

int S3BackupFileSystem::GetBlockSize()
{
	return S3_NOTIONAL_BLOCK_SIZE;
}

std::string S3BackupFileSystem::GetObjectURL(const std::string& ObjectURI) const
{
	const Configuration s3config = mrConfig.GetSubConfiguration("S3Store");
	return std::string("http://") + s3config.GetKeyValue("HostName") + ":" +
		s3config.GetKeyValue("Port") + ObjectURI;
}

int64_t S3BackupFileSystem::GetRevisionID(const std::string& uri,
	HTTPResponse& response) const
{
	std::string etag;

	if(!response.GetHeader("etag", &etag))
	{
		THROW_EXCEPTION_MESSAGE(BackupStoreException, MissingEtagHeader,
			"Failed to get the MD5 checksum of the file or directory "
			"at this URL: " << GetObjectURL(uri));
	}

	if(etag[0] != '"')
	{
		THROW_EXCEPTION_MESSAGE(BackupStoreException, InvalidEtagHeader,
			"Failed to get the MD5 checksum of the file or directory "
			"at this URL: " << GetObjectURL(uri));
	}

	char* pEnd = NULL;
	int64_t revID = box_strtoui64(etag.substr(1, 17).c_str(), &pEnd, 16);
	if(*pEnd != '\0')
	{
		THROW_SYS_ERROR("Failed to get the MD5 checksum of the file or "
			"directory at this URL: " << uri << ": invalid character '" <<
			*pEnd << "' in '" << etag << "'", BackupStoreException,
			InvalidEtagHeader);
	}

	return revID;
}

std::auto_ptr<BackupStoreInfo> S3BackupFileSystem::GetBackupStoreInfo(int32_t AccountID,
	bool ReadOnly)
{
	std::string info_uri = GetMetadataURI(S3_INFO_FILE_NAME);
	HTTPResponse response = GetObject(info_uri);

	std::string info_url = GetObjectURL(info_uri);
	mrClient.CheckResponse(response, std::string("No BackupStoreInfo file exists "
		"at this URL: ") + info_url);

	std::auto_ptr<BackupStoreInfo> info = BackupStoreInfo::Load(response, info_url,
		ReadOnly);

	// We don't actually use AccountID to distinguish accounts on S3 stores.
	if(info->GetAccountID() != S3_FAKE_ACCOUNT_ID)
	{
		THROW_FILE_ERROR("Found wrong account ID in store info: expected " <<
			BOX_FORMAT_ACCOUNT(S3_FAKE_ACCOUNT_ID) << " but found " <<
			BOX_FORMAT_ACCOUNT(info->GetAccountID()),
			info_url, BackupStoreException, BadStoreInfoOnLoad);
	}

	return info;
}

void S3BackupFileSystem::PutBackupStoreInfo(BackupStoreInfo& rInfo)
{
	if(rInfo.IsReadOnly())
	{
		THROW_EXCEPTION_MESSAGE(BackupStoreException, StoreInfoIsReadOnly,
			"Tried to save BackupStoreInfo when configured as read-only");
	}

	CollectInBufferStream out;
	rInfo.Save(out);
	out.SetForReading();

	std::string info_uri = GetMetadataURI(S3_INFO_FILE_NAME);
	HTTPResponse response = PutObject(info_uri, out);

	std::string info_url = GetObjectURL(info_uri);
	mrClient.CheckResponse(response, std::string("Failed to upload the new "
		"BackupStoreInfo file to this URL: ") + info_url);
}

//! Returns whether an object (a file or directory) exists with this object ID, and its
//! revision ID, which for a RaidFile is based on its timestamp and file size.
bool S3BackupFileSystem::ObjectExists(int64_t ObjectID, int64_t *pRevisionID)
{
	std::string uri = GetDirectoryURI(ObjectID);
	HTTPResponse response = mrClient.HeadObject(uri);

	if(response.GetResponseCode() == HTTPResponse::Code_NotFound)
	{
		// A file might exist, check that too.
		uri = GetFileURI(ObjectID);
		response = mrClient.HeadObject(uri);
	}

	if(response.GetResponseCode() == HTTPResponse::Code_NotFound)
	{
		return false;
	}

	if(response.GetResponseCode() != HTTPResponse::Code_OK)
	{
		// Throw an appropriate exception.
		mrClient.CheckResponse(response, std::string("Failed to check whether "
			"a file or directory exists at this URL: ") +
			GetObjectURL(uri));
	}

	if(pRevisionID)
	{
		*pRevisionID = GetRevisionID(uri, response);
	}

	return true;
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    S3BackupFileSystem::GetObjectURI(int64_t ObjectID,
//			 int Type)
//		Purpose: Builds the object filename for a given object,
//			 including mBasePath. Very similar to
//			 StoreStructure::MakeObjectFilename(), but files and
//			 directories have different extensions, and the
//			 filename is the full object ID, not just the lower
//			 STORE_ID_SEGMENT_LENGTH bits.
//		Created: 2016/03/21
//
// --------------------------------------------------------------------------
std::string S3BackupFileSystem::GetObjectURI(int64_t ObjectID, int Type)
{
	const static char *hex = "0123456789abcdef";
	std::ostringstream out;
	ASSERT(mBasePath.size() > 0 && mBasePath[0] == '/' &&
		mBasePath[mBasePath.size() - 1] == '/');
	out << mBasePath;

	// Get the id value from the stored object ID into an unsigned int64_t, so that
	// we can do bitwise operations on it safely.
	uint64_t id = (uint64_t)ObjectID;

	// Shift off the bits which make up the leafname
	id >>= STORE_ID_SEGMENT_LENGTH;

	// build pathname
	while(id != 0)
	{
		// assumes that the segments are no bigger than 8 bits
		int v = id & STORE_ID_SEGMENT_MASK;
		out << hex[(v & 0xf0) >> 4];
		out << hex[v & 0xf];
		out << "/";

		// shift the bits we used off the pathname
		id >>= STORE_ID_SEGMENT_LENGTH;
	}

	// append the filename
	out << BOX_FORMAT_OBJECTID(ObjectID);
	if(Type == ObjectExists_File)
	{
		out << ".file";
	}
	else if(Type == ObjectExists_Dir)
	{
		out << ".dir";
	}
	else
	{
		THROW_EXCEPTION_MESSAGE(CommonException, Internal,
			"Unknown file type for object " << BOX_FORMAT_OBJECTID(ObjectID) <<
			": " << Type);
	}

	return out.str();
}

//! Reads a directory with the specified ID into the supplied BackupStoreDirectory
//! object, also initialising its revision ID and SizeInBlocks.
void S3BackupFileSystem::GetDirectory(int64_t ObjectID, BackupStoreDirectory& rDirOut)
{
	std::string uri = GetDirectoryURI(ObjectID);
	HTTPResponse response = GetObject(uri);
	mrClient.CheckResponse(response,
		std::string("Failed to download directory: ") + uri);
	rDirOut.ReadFromStream(response, mrClient.GetNetworkTimeout());

	rDirOut.SetRevisionID(GetRevisionID(uri, response));
	rDirOut.SetUserInfo1_SizeInBlocks(GetSizeInBlocks(response.GetContentLength()));
}

//! Writes the supplied BackupStoreDirectory object to the store, and updates its revision
//! ID and SizeInBlocks.
void S3BackupFileSystem::PutDirectory(BackupStoreDirectory& rDir)
{
	CollectInBufferStream out;
	rDir.WriteToStream(out);
	out.SetForReading();

	std::string uri = GetDirectoryURI(rDir.GetObjectID());
	HTTPResponse response = PutObject(uri, out);
	mrClient.CheckResponse(response,
		std::string("Failed to upload directory: ") + uri);

	rDir.SetRevisionID(GetRevisionID(uri, response));
	rDir.SetUserInfo1_SizeInBlocks(GetSizeInBlocks(out.GetSize()));
}

