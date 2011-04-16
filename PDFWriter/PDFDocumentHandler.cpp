#include "PDFDocumentHandler.h"
#include "Trace.h"
#include "RefCountPtr.h"
#include "PDFObjectCast.h"
#include "PDFArray.h"
#include "PDFInteger.h"
#include "PDFReal.h"
#include "PDFDictionary.h"
#include "DocumentContext.h"
#include "PDFFormXObject.h"
#include "PDFStream.h"
#include "OutputStreamTraits.h"
#include "PDFStreamInput.h"
#include "OutputFlateDecodeStream.h"
#include "ObjectsContext.h"
#include "PDFIndirectObjectReference.h"
#include "PDFBoolean.h"
#include "PDFSymbol.h"
#include "DictionaryContext.h"
#include "PDFLiteralString.h"
#include "PDFHexString.h"
#include "PrimitiveObjectsWriter.h"
#include "PageContentContext.h"
#include "PDFPage.h"


PDFDocumentHandler::PDFDocumentHandler(void)
{
	mObjectsContext = NULL;
	mDocumentContext = NULL;
	mWrittenPage = NULL;
}

PDFDocumentHandler::~PDFDocumentHandler(void)
{
}

void PDFDocumentHandler::SetOperationsContexts(DocumentContext* inDocumentContext,ObjectsContext* inObjectsContext)
{
	mObjectsContext = inObjectsContext;
	mDocumentContext = inDocumentContext;
}




EStatusCodeAndPDFFormXObjectList PDFDocumentHandler::CreateFormXObjectsFromPDF(	const wstring& inPDFFilePath,
																				const PDFPageRange& inPageRange,
																				EPDFPageBox inPageBoxToUseAsFormBox,
																				const double* inTransformationMatrix)
{
	EStatusCodeAndPDFFormXObjectList result;

	do
	{
		result.first = mPDFFile.OpenFile(inPDFFilePath);
		if(result.first != eSuccess)
		{
			TRACE_LOG1("PDFDocumentHandler::CreateFormXObjectsFromPDF, unable to open file for reading in %s",inPDFFilePath.c_str());
			break;
		}

		result.first = mParser.StartPDFParsing(mPDFFile.GetInputStream());
		if(result.first != eSuccess)
		{
			TRACE_LOG("PDFDocumentHandler::CreateFormXObjectsFromPDF, failure occured while parsing PDF file.");
			break;
		}

		PDFFormXObject* newObject;

		if(PDFPageRange::eRangeTypeAll == inPageRange.mType)
		{
			for(unsigned long i=0; i < mParser.GetPagesCount() && eSuccess == result.first; ++i)
			{
				newObject = CreatePDFFormXObjectForPage(i,inPageBoxToUseAsFormBox,inTransformationMatrix);
				if(newObject)
				{
					result.second.push_back(newObject);
				}
				else
				{
					TRACE_LOG1("PDFDocumentHandler::CreateFormXObjectsFromPDF, failed to embed page %ld", i);
					result.first = eFailure;
				}
			}
		}
		else
		{
			// eRangeTypeSpecific
			ULongAndULongList::const_iterator it = inPageRange.mSpecificRanges.begin();
			for(; it != inPageRange.mSpecificRanges.end() && eSuccess == result.first;++it)
			{
				if(it->first <= it->second && it->second < mParser.GetPagesCount())
				{
					for(unsigned long i=it->first; i <= it->second && eSuccess == result.first; ++i)
					{
						newObject = CreatePDFFormXObjectForPage(i,inPageBoxToUseAsFormBox,inTransformationMatrix);
						if(newObject)
						{
							result.second.push_back(newObject);
						}
						else
						{
							TRACE_LOG1("PDFDocumentHandler::CreateFormXObjectsFromPDF, failed to embed page %ld", i);
							result.first = eFailure;
						}
					}
				}
				else
				{
					TRACE_LOG3("PDFDocumentHandler::CreateFormXObjectsFromPDF, range mismatch. first = %ld, second = %ld, PDF page count = %ld", 
						it->first,
						it->second,
						mParser.GetPagesCount());
					result.first = eFailure;
				}
			}
		}


	}while(false);

	mPDFFile.CloseFile();
	// clearing the source to target mapping here. note that pages copying enjoyed sharing of objects between them
	mSourceToTarget.clear();

	return result;

}

PDFFormXObject* PDFDocumentHandler::CreatePDFFormXObjectForPage(unsigned long inPageIndex,
																EPDFPageBox inPageBoxToUseAsFormBox,
																const double* inTransformationMatrix)
{
	RefCountPtr<PDFDictionary> pageObject = mParser.ParsePage(inPageIndex);
	PDFFormXObject* result = NULL;

	if(!pageObject)
	{
		TRACE_LOG1("PDFDocumentHandler::CreatePDFFormXObjectForPage, unhexpected exception, page index does not denote a page object. page index = %ld",inPageIndex);
		return NULL;
	}

	do
	{
		if(CopyResourcesIndirectObjects(pageObject.GetPtr()) != eSuccess)
			break;

		// Create a new form XObject
		result = mDocumentContext->StartFormXObject(DeterminePageBox(pageObject.GetPtr(),inPageBoxToUseAsFormBox),inTransformationMatrix);

		// copy the page content to the target XObject stream
		if(WritePageContentToSingleStream(result->GetContentStream()->GetWriteStream(),pageObject.GetPtr()) != eSuccess)
		{
			delete result;
			result = NULL;
			break;
		}

		// resources dictionary is gonna be empty at this point...so we can use our own code to write the dictionary, by extending.
		// which will be a simple loop. note that at this point all indirect objects should have been copied, and have mapping
		mDocumentContext->AddDocumentContextExtender(this);
		mWrittenPage = pageObject.GetPtr();

		if(mDocumentContext->EndFormXObjectNoRelease(result) != eSuccess)
		{
			delete result;
			result = NULL;
			break;
		}

	}while(false);

	mWrittenPage = NULL;
	mDocumentContext->RemoveDocumentContextExtender(this);

	return result;

}

PDFRectangle PDFDocumentHandler::DeterminePageBox(PDFDictionary* inDictionary,EPDFPageBox inPageBoxType)
{
	PDFRectangle result;

	switch(inPageBoxType)
	{
		case ePDFPageBoxMediaBox:
		{
			PDFObjectCastPtr<PDFArray> mediaBox(QueryInheritedValue(inDictionary,"MediaBox"));
			if(!mediaBox || mediaBox->GetLength() != 4)
			{
				TRACE_LOG("PDFDocumentHandler::DeterminePageBox, Exception, pdf page does not have correct media box. defaulting to A4");
				result = PDFRectangle(0,0,595,842);
			}
			else
			{
				SetPDFRectangleFromPDFArray(mediaBox.GetPtr(),result);
			}
			break;
		}
		case ePDFPageBoxCropBox:
		{
			PDFObjectCastPtr<PDFArray> cropBox(QueryInheritedValue(inDictionary,"CropBox"));
			
			if(!cropBox || cropBox->GetLength() != 4)
			{
				TRACE_LOG("PDFDocumentHandler::DeterminePageBox, PDF does not have crop box, defaulting to media box.");
				result = DeterminePageBox(inDictionary,ePDFPageBoxMediaBox);
			}
			else
			{
				SetPDFRectangleFromPDFArray(cropBox.GetPtr(),result);
			}
			break;
		}
		case ePDFPageBoxBleedBox:
		{
			PDFObjectCastPtr<PDFArray> bleedBox(mParser.QueryDictionaryObject(inDictionary,"BleedBox"));
			if(!bleedBox || bleedBox->GetLength() != 4)
			{
				TRACE_LOG("PDFDocumentHandler::DeterminePageBox, PDF does not have bleed box, defaulting to crop box.");
				result = DeterminePageBox(inDictionary,ePDFPageBoxCropBox);
			}
			else
			{
				SetPDFRectangleFromPDFArray(bleedBox.GetPtr(),result);
			}
			break;
		}
		case ePDFPageBoxTrimBox:
		{
			PDFObjectCastPtr<PDFArray> trimBox(mParser.QueryDictionaryObject(inDictionary,"TrimBox"));
			if(!trimBox || trimBox->GetLength() != 4)
			{
				TRACE_LOG("PDFDocumentHandler::DeterminePageBox, PDF does not have trim box, defaulting to crop box.");
				result = DeterminePageBox(inDictionary,ePDFPageBoxCropBox);
			}
			else
			{
				SetPDFRectangleFromPDFArray(trimBox.GetPtr(),result);
			}
			break;
		}
		case ePDFPageBoxArtBox:
		{
			PDFObjectCastPtr<PDFArray> artBox(mParser.QueryDictionaryObject(inDictionary,"ArtBox"));
			if(!artBox || artBox->GetLength() != 4)
			{
				TRACE_LOG("PDFDocumentHandler::DeterminePageBox, PDF does not have art box, defaulting to crop box.");
				result = DeterminePageBox(inDictionary,ePDFPageBoxCropBox);
			}
			else
			{
				SetPDFRectangleFromPDFArray(artBox.GetPtr(),result);
			}
			break;
		}
	}
	return result;
}

void PDFDocumentHandler::SetPDFRectangleFromPDFArray(PDFArray* inPDFArray,PDFRectangle& outPDFRectangle)
{
	RefCountPtr<PDFObject> lowerLeftX(inPDFArray->QueryObject(0));
	RefCountPtr<PDFObject> lowerLeftY(inPDFArray->QueryObject(1));
	RefCountPtr<PDFObject> upperRightX(inPDFArray->QueryObject(2));
	RefCountPtr<PDFObject> upperRightY(inPDFArray->QueryObject(3));
	
	outPDFRectangle.LowerLeftX = GetAsDoubleValue(lowerLeftX.GetPtr());
	outPDFRectangle.LowerLeftY = GetAsDoubleValue(lowerLeftY.GetPtr());
	outPDFRectangle.UpperRightX = GetAsDoubleValue(upperRightX.GetPtr());
	outPDFRectangle.UpperRightY = GetAsDoubleValue(upperRightY.GetPtr());
}

double PDFDocumentHandler::GetAsDoubleValue(PDFObject* inNumberObject)
{
	if(inNumberObject->GetType() == ePDFObjectInteger)
	{
		PDFInteger* anInteger = (PDFInteger*)inNumberObject;
		return (double)anInteger->GetValue();
	}
	else if(inNumberObject->GetType() == ePDFObjectReal)
	{
		PDFReal* aReal = (PDFReal*)inNumberObject;
		return aReal->GetValue();
	}
	else
		return 0;
}

EStatusCode PDFDocumentHandler::WritePageContentToSingleStream(IByteWriter* inTargetStream,PDFDictionary* inPageObject)
{
	EStatusCode status = eSuccess;

	RefCountPtr<PDFObject> pageContent(mParser.QueryDictionaryObject(inPageObject,"Contents"));
	if(pageContent->GetType() == ePDFObjectStream)
	{
		status = WritePDFStreamInputToStream(inTargetStream,(PDFStreamInput*)pageContent.GetPtr());
	}
	else if(pageContent->GetType() == ePDFObjectArray)
	{
		SingleValueContainerIterator<PDFObjectVector> it = ((PDFArray*)pageContent.GetPtr())->GetIterator();
		PDFObjectCastPtr<PDFIndirectObjectReference> refItem;
		while(it.MoveNext() && status == eSuccess)
		{
			refItem = it.GetItem();
			if(!refItem)
			{
				status = eFailure;
				TRACE_LOG("PDFDocumentHandler::WritePageContentToSingleStream, content stream array contains non-refs");
				break;
			}
			PDFObjectCastPtr<PDFStreamInput> contentStream(mParser.ParseNewObject(refItem->mObjectID));
			if(!contentStream)
			{
				status = eFailure;
				TRACE_LOG("PDFDocumentHandler::WritePageContentToSingleStream, content stream array contains references to non streams");
				break;
			}
			status = WritePDFStreamInputToStream(inTargetStream,contentStream.GetPtr());
			if(eSuccess == status)
			{
				// separate the streams with a nice endline
				PrimitiveObjectsWriter primitivesWriter;
				primitivesWriter.SetStreamForWriting(inTargetStream);
				primitivesWriter.EndLine();
			}
		}
	}
	else
	{
		TRACE_LOG1("PDFDocumentHandler::WritePageContentToSingleStream, error copying page content, expected either array or stream, getting %s",scPDFObjectTypeLabel[pageContent->GetType()]);
		status = eFailure;
	}
	

	return status;
}

EStatusCode PDFDocumentHandler::WritePDFStreamInputToStream(IByteWriter* inTargetStream,PDFStreamInput* inSourceStream)
{
	RefCountPtr<PDFDictionary> streamDictionary(inSourceStream->QueryStreamDictionary());
	PDFObjectCastPtr<PDFInteger> lengthObject(mParser.QueryDictionaryObject(streamDictionary.GetPtr(),"Length"));	

	if(!lengthObject)
	{
		TRACE_LOG("PDFDocumentHandler::WritePDFStreamInputToStream, stream does not have length, failing");
		return eFailure;
	}

	RefCountPtr<PDFObject> filterObject(mParser.QueryDictionaryObject(streamDictionary.GetPtr(),"Filter"));

	mPDFFile.GetInputStream()->SetPosition(inSourceStream->GetStreamContentStart());
	if(!filterObject)
	{
		OutputStreamTraits traits(inTargetStream);
		return traits.CopyToOutputStream(mPDFFile.GetInputStream(),(LongBufferSizeType)lengthObject->GetValue());	
	}
	else if(filterObject->GetType() == ePDFObjectName && ((PDFName*)(filterObject.GetPtr()))->GetValue() == "FlateDecode")
	{
		OutputFlateDecodeStream decoder(inTargetStream);
		OutputStreamTraits traits(&decoder);
		EStatusCode status = traits.CopyToOutputStream(mPDFFile.GetInputStream(),(LongBufferSizeType)lengthObject->GetValue());	

		// must assign null, cause decoder takes ownership
		decoder.Assign(NULL);
		return status;
	}
	else
	{
		TRACE_LOG("PDFDocumentHandler::WritePDFStreamInputToStream, can only handle unencoded or flate streams. sorry");
		return eFailure;
	}
}

EStatusCode PDFDocumentHandler::CopyResourcesIndirectObjects(PDFDictionary* inPage)
{
	// makes sure that all indirect references are copied. those will come from the resources dictionary.
	// this is how we go about this:
	// Loop the immediate entities of the resources dictionary. for each value (which may be indirect) do this:
	// if indirect, run CopyInDirectObject on it (passing its ID and a new ID at the target PDF (just allocate as you go))
	// if direct, let go.

	PDFObjectCastPtr<PDFDictionary> resources(mParser.QueryDictionaryObject(inPage,"Resources"));

	// k. no resources...as wierd as that might be...or just wrong...i'll let it be
	if(!resources)
		return eSuccess;

	ObjectIDTypeList newObjectsToWrite;
	ObjectIDTypeSet writtenObjects;

	RegisterInDirectObjects(resources.GetPtr(),newObjectsToWrite);

	return WriteNewObjects(newObjectsToWrite,writtenObjects);
}

EStatusCode PDFDocumentHandler::WriteNewObjects(const ObjectIDTypeList& inSourceObjectIDs,ObjectIDTypeSet& ioCopiedObjects)
{

	ObjectIDTypeList::const_iterator itNewObjects = inSourceObjectIDs.begin();
	EStatusCode status = eSuccess;

	for(; itNewObjects != inSourceObjectIDs.end() && eSuccess == status; ++itNewObjects)
	{
		// theoretically speaking, it could be that while one object was copied, another one in this array is already
		// copied, so make sure to check that these objects are still required for copying
		if(ioCopiedObjects.find(*itNewObjects) == ioCopiedObjects.end())
		{
			ObjectIDTypeToObjectIDTypeMap::iterator it = mSourceToTarget.find(*itNewObjects);
			if(it == mSourceToTarget.end())
			{
				ObjectIDType newObjectID = mObjectsContext->GetInDirectObjectsRegistry().AllocateNewObjectID();
				it = mSourceToTarget.insert(ObjectIDTypeToObjectIDTypeMap::value_type(*itNewObjects,newObjectID)).first;
			}
			ioCopiedObjects.insert(*itNewObjects);
			status = CopyInDirectObject(*itNewObjects,it->second,ioCopiedObjects);
		}
	}
	return status;
}

void PDFDocumentHandler::RegisterInDirectObjects(PDFDictionary* inDictionary,ObjectIDTypeList& outNewObjects)
{
	MapIterator<PDFNameToPDFObjectMap> it(inDictionary->GetIterator());

	// i'm assuming keys are directs. i can move into indirects if that's important
	while(it.MoveNext())
	{
		if(it.GetValue()->GetType() == ePDFObjectIndirectObjectReference)
		{
			ObjectIDTypeToObjectIDTypeMap::iterator	itObjects = mSourceToTarget.find(((PDFIndirectObjectReference*)it.GetValue())->mObjectID);
			if(itObjects == mSourceToTarget.end())
				outNewObjects.push_back(((PDFIndirectObjectReference*)it.GetValue())->mObjectID);
		} 
		else if(it.GetValue()->GetType() == ePDFObjectArray)
		{
			RegisterInDirectObjects((PDFArray*)it.GetValue(),outNewObjects);
		}
		else if(it.GetValue()->GetType() == ePDFObjectDictionary)
		{
			RegisterInDirectObjects((PDFDictionary*)it.GetValue(),outNewObjects);
		}
	}
}

void PDFDocumentHandler::RegisterInDirectObjects(PDFArray* inArray,ObjectIDTypeList& outNewObjects)
{
	SingleValueContainerIterator<PDFObjectVector> it(inArray->GetIterator());

	while(it.MoveNext())
	{
		if(it.GetItem()->GetType() == ePDFObjectIndirectObjectReference)
		{
			ObjectIDTypeToObjectIDTypeMap::iterator	itObjects = mSourceToTarget.find(((PDFIndirectObjectReference*)it.GetItem())->mObjectID);
			if(itObjects == mSourceToTarget.end())
				outNewObjects.push_back(((PDFIndirectObjectReference*)it.GetItem())->mObjectID);
		} 
		else if(it.GetItem()->GetType() == ePDFObjectArray)
		{
			RegisterInDirectObjects((PDFArray*)it.GetItem(),outNewObjects);
		}
		else if(it.GetItem()->GetType() == ePDFObjectDictionary)
		{
			RegisterInDirectObjects((PDFDictionary*)it.GetItem(),outNewObjects);
		}
	}
}

EStatusCode PDFDocumentHandler::CopyInDirectObject(ObjectIDType inSourceObjectID,ObjectIDType inTargetObjectID,ObjectIDTypeSet& ioCopiedObjects)
{
	// CopyInDirectObject will do this (lissen up)
	// Start a new object with the input ID
	// Write the object according to type. For arrays/dicts there might be indirect objects to copy. for them do this:
	// if you got it in the map already, just write down the new ID. if not register the ID, with a new ID already allocated at this point.
	// Once done. loop what you registered, using CopyInDirectObject in the same manner. This should maintain that each object is written separately
	EStatusCode status;
	ObjectIDTypeList newObjectsToWrite;

	RefCountPtr<PDFObject> sourceObject = mParser.ParseNewObject(inSourceObjectID);
	if(!sourceObject)
	{
		TRACE_LOG1("PDFDocumentHandler::CopyInDirectObject, object not found. %ld",inSourceObjectID);
		return eFailure;
	}

	mObjectsContext->StartNewIndirectObject(inTargetObjectID);
	status = WriteObjectByType(sourceObject.GetPtr(),eTokenSeparatorEndLine,newObjectsToWrite);
	if(eSuccess == status)
	{
		mObjectsContext->EndIndirectObject();
		return WriteNewObjects(newObjectsToWrite,ioCopiedObjects);
	}
	else
		return status;
}

EStatusCode PDFDocumentHandler::WriteObjectByType(PDFObject* inObject,ETokenSeparator inSeparator,ObjectIDTypeList& outSourceObjectsToAdd)
{
	EStatusCode status = eSuccess;

	switch(inObject->GetType())
	{
		case ePDFObjectBoolean:
		{
			mObjectsContext->WriteBoolean(((PDFBoolean*)inObject)->GetValue(),inSeparator);
			break;
		}
		case ePDFObjectLiteralString:
		{
			mObjectsContext->WriteLiteralString(((PDFLiteralString*)inObject)->GetValue(),inSeparator);
			break;
		}
		case ePDFObjectHexString:
		{
			mObjectsContext->WriteHexString(((PDFHexString*)inObject)->GetValue(),inSeparator);
			break;
		}
		case ePDFObjectNull:
		{
			mObjectsContext->WriteNull(eTokenSeparatorEndLine);
			break;
		}
		case ePDFObjectName:
		{
			mObjectsContext->WriteName(((PDFName*)inObject)->GetValue(),inSeparator);
			break;
		}
		case ePDFObjectInteger:
		{
			mObjectsContext->WriteInteger(((PDFInteger*)inObject)->GetValue(),inSeparator);
			break;
		}
		case ePDFObjectReal:
		{
			mObjectsContext->WriteDouble(((PDFReal*)inObject)->GetValue(),inSeparator);
			break;
		}
		case ePDFObjectSymbol:
		{
			mObjectsContext->WriteKeyword(((PDFSymbol*)inObject)->GetValue());
			break;
		}
		case ePDFObjectIndirectObjectReference:
		{
			ObjectIDType sourceObjectID = ((PDFIndirectObjectReference*)inObject)->mObjectID;
			ObjectIDTypeToObjectIDTypeMap::iterator	itObjects = mSourceToTarget.find(sourceObjectID);
			if(itObjects == mSourceToTarget.end())
			{
				ObjectIDType newObjectID = mObjectsContext->GetInDirectObjectsRegistry().AllocateNewObjectID();
				itObjects = mSourceToTarget.insert(ObjectIDTypeToObjectIDTypeMap::value_type(sourceObjectID,newObjectID)).first;
				outSourceObjectsToAdd.push_back(sourceObjectID);
			}
			mObjectsContext->WriteIndirectObjectReference(itObjects->second,inSeparator);
			break;
		}
		case ePDFObjectArray:
		{
			status = WriteArrayObject((PDFArray*)inObject,inSeparator,outSourceObjectsToAdd);
			break;
		}
		case ePDFObjectDictionary:
		{
			status = WriteDictionaryObject((PDFDictionary*)inObject,outSourceObjectsToAdd);
			break;
		}
		case ePDFObjectStream:
		{
			status = WriteStreamObject((PDFStreamInput*)inObject,outSourceObjectsToAdd);
			break;
		}
	}
	return status;
}


EStatusCode PDFDocumentHandler::WriteArrayObject(PDFArray* inArray,ETokenSeparator inSeparator,ObjectIDTypeList& outSourceObjectsToAdd)
{
	SingleValueContainerIterator<PDFObjectVector> it(inArray->GetIterator());

	EStatusCode status = eSuccess;
	
	mObjectsContext->StartArray();

	while(it.MoveNext() && eSuccess == status)
		status = WriteObjectByType(it.GetItem(),eTokenSeparatorSpace,outSourceObjectsToAdd);

	if(eSuccess == status)
		mObjectsContext->EndArray(inSeparator);

	return status;
}



EStatusCode PDFDocumentHandler::WriteDictionaryObject(PDFDictionary* inDictionary,ObjectIDTypeList& outSourceObjectsToAdd)
{
	MapIterator<PDFNameToPDFObjectMap> it(inDictionary->GetIterator());
	EStatusCode status = eSuccess;
	DictionaryContext* dictionary = mObjectsContext->StartDictionary();

	while(it.MoveNext() && eSuccess == status)
	{
		status = dictionary->WriteKey(it.GetKey()->GetValue());
		if(eSuccess == status)
			status = WriteObjectByType(it.GetValue(),dictionary,outSourceObjectsToAdd);
	}
	
	if(eSuccess == status)
	{
		return mObjectsContext->EndDictionary(dictionary);
	}
	else
		return eSuccess;
}

EStatusCode PDFDocumentHandler::WriteObjectByType(PDFObject* inObject,DictionaryContext* inDictionaryContext,ObjectIDTypeList& outSourceObjectsToAdd)
{
	EStatusCode status = eSuccess;

	switch(inObject->GetType())
	{
		case ePDFObjectBoolean:
		{
			inDictionaryContext->WriteBooleanValue(((PDFBoolean*)inObject)->GetValue());
			break;
		}
		case ePDFObjectLiteralString:
		{
			inDictionaryContext->WriteLiteralStringValue(((PDFLiteralString*)inObject)->GetValue());
			break;
		}
		case ePDFObjectHexString:
		{
			inDictionaryContext->WriteHexStringValue(((PDFHexString*)inObject)->GetValue());
			break;
		}
		case ePDFObjectNull:
		{
			inDictionaryContext->WriteNullValue();
			break;
		}
		case ePDFObjectName:
		{
			inDictionaryContext->WriteNameValue(((PDFName*)inObject)->GetValue());
			break;
		}
		case ePDFObjectInteger:
		{
			inDictionaryContext->WriteIntegerValue(((PDFInteger*)inObject)->GetValue());
			break;
		}
		case ePDFObjectReal:
		{
			inDictionaryContext->WriteDoubleValue(((PDFReal*)inObject)->GetValue());
			break;
		}
		case ePDFObjectSymbol:
		{
			// not sure this is supposed to happen...but then...it is probably not supposed to happen at any times...
			inDictionaryContext->WriteKeywordValue(((PDFSymbol*)inObject)->GetValue());
			break;
		}
		case ePDFObjectIndirectObjectReference:
		{
			ObjectIDType sourceObjectID = ((PDFIndirectObjectReference*)inObject)->mObjectID;
			ObjectIDTypeToObjectIDTypeMap::iterator	itObjects = mSourceToTarget.find(sourceObjectID);
			if(itObjects == mSourceToTarget.end())
			{
				ObjectIDType newObjectID = mObjectsContext->GetInDirectObjectsRegistry().AllocateNewObjectID();
				itObjects = mSourceToTarget.insert(ObjectIDTypeToObjectIDTypeMap::value_type(sourceObjectID,newObjectID)).first;
				outSourceObjectsToAdd.push_back(sourceObjectID);
			}
			inDictionaryContext->WriteObjectReferenceValue(itObjects->second);
			break;
		}
		case ePDFObjectArray:
		{
			status = WriteArrayObject((PDFArray*)inObject,eTokenSeparatorEndLine,outSourceObjectsToAdd);
			break;
		}
		case ePDFObjectDictionary:
		{
			status = WriteDictionaryObject((PDFDictionary*)inObject,outSourceObjectsToAdd);
			break;
		}
		case ePDFObjectStream:
		{
			// k. that's not supposed to happen
			TRACE_LOG("PDFDocumentHandler::WriteObjectByType, got that wrong sir. ain't gonna write a stream in a dictionary. must be a ref. we got exception here");
			break;
		}
	}
	return status;
}

EStatusCode PDFDocumentHandler::WriteStreamObject(PDFStreamInput* inStream,ObjectIDTypeList& outSourceObjectsToAdd)
{
	// i'm going to copy the stream directly, cause i don't need all this transcoding and such. if we ever do, i'll write a proper
	// PDFStream implementation.
	RefCountPtr<PDFDictionary> streamDictionary(inStream->QueryStreamDictionary());

	if(WriteDictionaryObject(streamDictionary.GetPtr(),outSourceObjectsToAdd) != eSuccess)
	{
		TRACE_LOG("PDFDocumentHandler::WriteStreamObject, failed to write stream dictionary");
		return eFailure;
	}

	mObjectsContext->WriteKeyword("stream");


	PDFObjectCastPtr<PDFInteger> lengthObject(mParser.QueryDictionaryObject(streamDictionary.GetPtr(),"Length"));	

	if(!lengthObject)
	{
		TRACE_LOG("PDFDocumentHandler::WriteStreamObject, stream does not have length, failing");
		return eFailure;
	}

	mPDFFile.GetInputStream()->SetPosition(inStream->GetStreamContentStart());

	OutputStreamTraits traits(mObjectsContext->StartFreeContext());
	EStatusCode status = traits.CopyToOutputStream(mPDFFile.GetInputStream(),(LongBufferSizeType)lengthObject->GetValue());	
	if(eSuccess == status)
	{
		mObjectsContext->EndFreeContext();
		mObjectsContext->EndLine(); // this one just to make sure
		mObjectsContext->WriteKeyword("endstream");
	}
	return status;
}

EStatusCode PDFDocumentHandler::OnResourcesWrite(
						ResourcesDictionary* inResources,
						DictionaryContext* inPageResourcesDictionaryContext,
						ObjectsContext* inPDFWriterObjectContext,
						DocumentContext* inPDFWriterDocumentContext)
{
	// Writing resources dictionary. simply loop internal elements and copy. nicely enough, i can use read methods, trusting
	// that no new objects need be written
	
	PDFObjectCastPtr<PDFDictionary> resources(mParser.QueryDictionaryObject(mWrittenPage,"Resources"));
	ObjectIDTypeList dummyObjectList; // this one should remain empty...

	// k. no resources...as wierd as that might be...or just wrong...i'll let it be
	if(!resources)
		return eSuccess;

	MapIterator<PDFNameToPDFObjectMap> it(resources->GetIterator());
	EStatusCode status = eSuccess;

	while(it.MoveNext() && eSuccess == status)
	{
		status = inPageResourcesDictionaryContext->WriteKey(it.GetKey()->GetValue());
		if(eSuccess == status)
			status = WriteObjectByType(it.GetValue(),inPageResourcesDictionaryContext,dummyObjectList);
	}
	return status;
}

EStatusCodeAndObjectIDTypeList PDFDocumentHandler::AppendPDFPagesFromPDF(const wstring& inPDFFilePath,
																		const PDFPageRange& inPageRange)
{
	EStatusCodeAndObjectIDTypeList result;
	

	do
	{
		result.first = mPDFFile.OpenFile(inPDFFilePath);
		if(result.first != eSuccess)
		{
			TRACE_LOG1("PDFDocumentHandler::CreatePDFPagesFromPDF, unable to open file for reading in %s",inPDFFilePath.c_str());
			break;
		}

		result.first = mParser.StartPDFParsing(mPDFFile.GetInputStream());
		if(result.first != eSuccess)
		{
			TRACE_LOG("PDFDocumentHandler::CreatePDFPagesFromPDF, failure occured while parsing PDF file.");
			break;
		}

		EStatusCodeAndObjectIDType newObject;

		if(PDFPageRange::eRangeTypeAll == inPageRange.mType)
		{
			for(unsigned long i=0; i < mParser.GetPagesCount() && eSuccess == result.first; ++i)
			{
				newObject = CreatePDFPageForPage(i);
				if(eSuccess == newObject.first)
				{
					result.second.push_back(newObject.second);
				}
				else
				{
					TRACE_LOG1("PDFDocumentHandler::CreatePDFPagesFromPDF, failed to embed page %ld", i);
					result.first = eFailure;
				}
			}
		}
		else
		{
			// eRangeTypeSpecific
			ULongAndULongList::const_iterator it = inPageRange.mSpecificRanges.begin();
			for(; it != inPageRange.mSpecificRanges.end() && eSuccess == result.first;++it)
			{
				if(it->first <= it->second && it->second < mParser.GetPagesCount())
				{
					for(unsigned long i=it->first; i <= it->second && eSuccess == result.first; ++i)
					{
						newObject = CreatePDFPageForPage(i);
						if(eSuccess == newObject.first)
						{
							result.second.push_back(newObject.second);
						}
						else
						{
							TRACE_LOG1("PDFDocumentHandler::CreatePDFPagesFromPDF, failed to embed page %ld", i);
							result.first = eFailure;
						}
					}
				}
				else
				{
					TRACE_LOG3("PDFDocumentHandler::CreatePDFPagesFromPDF, range mismatch. first = %ld, second = %ld, PDF page count = %ld", 
						it->first,
						it->second,
						mParser.GetPagesCount());
					result.first = eFailure;
				}
			}
		}


	}while(false);

	mPDFFile.CloseFile();
	// clearing the source to target mapping here. note that pages copying enjoyed sharing of objects between them
	mSourceToTarget.clear();

	return result;
}

EStatusCodeAndObjectIDType PDFDocumentHandler::CreatePDFPageForPage(unsigned long inPageIndex)
{
	RefCountPtr<PDFDictionary> pageObject = mParser.ParsePage(inPageIndex);
	EStatusCodeAndObjectIDType result;
	result.first = eFailure;
	result.second = 0;
	PDFPage* newPage = NULL;

	if(!pageObject)
	{
		TRACE_LOG1("PDFDocumentHandler::CreatePDFPageForPage, unhexpected exception, page index does not denote a page object. page index = %ld",inPageIndex);
		return result;
	}

	do
	{
		if(CopyResourcesIndirectObjects(pageObject.GetPtr()) != eSuccess)
			break;

		// Create a new form XObject
		newPage = new PDFPage();
		newPage->SetMediaBox(DeterminePageBox(pageObject.GetPtr(),ePDFPageBoxMediaBox));

		// copy the page content to the target page content
		if(CopyPageContentToTargetPage(newPage,pageObject.GetPtr()) != eSuccess)
		{
			delete newPage;
			newPage = NULL;
			break;
		}

		// resources dictionary is gonna be empty at this point...so we can use our own code to write the dictionary, by extending.
		// which will be a simple loop. note that at this point all indirect objects should have been copied, and have mapping
		mDocumentContext->AddDocumentContextExtender(this);
		mWrittenPage = pageObject.GetPtr();

		result = mDocumentContext->WritePage(newPage);
		if(result.first != eSuccess)
		{
			delete newPage;
			newPage = NULL;
			break;
		}

	}while(false);

	delete newPage;
	mWrittenPage = NULL;
	mDocumentContext->RemoveDocumentContextExtender(this);

	return result;	
}

EStatusCode PDFDocumentHandler::CopyPageContentToTargetPage(PDFPage* inPage,PDFDictionary* inPageObject)
{
	EStatusCode status = eSuccess;

	PageContentContext* pageContentContext = mDocumentContext->StartPageContentContext(inPage);

	RefCountPtr<PDFObject> pageContent(mParser.QueryDictionaryObject(inPageObject,"Contents"));
	if(pageContent->GetType() == ePDFObjectStream)
	{
		status = WritePDFStreamInputToContentContext(pageContentContext,(PDFStreamInput*)pageContent.GetPtr());
	}
	else if(pageContent->GetType() == ePDFObjectArray)
	{
		SingleValueContainerIterator<PDFObjectVector> it = ((PDFArray*)pageContent.GetPtr())->GetIterator();
		PDFObjectCastPtr<PDFIndirectObjectReference> refItem;
		while(it.MoveNext() && status == eSuccess)
		{
			refItem = it.GetItem();
			if(!refItem)
			{
				status = eFailure;
				TRACE_LOG("PDFDocumentHandler::CopyPageContentToTargetPage, content stream array contains non-refs");
				break;
			}
			PDFObjectCastPtr<PDFStreamInput> contentStream(mParser.ParseNewObject(refItem->mObjectID));
			if(!contentStream)
			{
				status = eFailure;
				TRACE_LOG("PDFDocumentHandler::CopyPageContentToTargetPage, content stream array contains references to non streams");
				break;
			}
			status = WritePDFStreamInputToContentContext(pageContentContext,contentStream.GetPtr());
		}
	}
	else
	{
		TRACE_LOG1("PDFDocumentHandler::CopyPageContentToTargetPage, error copying page content, expected either array or stream, getting %s",scPDFObjectTypeLabel[pageContent->GetType()]);
		status = eFailure;
	}
	
	if(status != eSuccess)
	{
		delete pageContentContext;
	}
	else
	{
		mDocumentContext->EndPageContentContext(pageContentContext);
	}
	return status;
}

EStatusCode PDFDocumentHandler::WritePDFStreamInputToContentContext(PageContentContext* inContentContext,PDFStreamInput* inContentSource)
{
	EStatusCode status = eSuccess;
	
	do
	{
		inContentContext->StartAStreamIfRequired();

		status = WritePDFStreamInputToStream(inContentContext->GetCurrentPageContentStream()->GetWriteStream(),inContentSource);
		if(status != eSuccess)
		{
			TRACE_LOG("PDFDocumentHandler::WritePDFStreamInputToContentContext, failed to write content stream from page input to target page");
			break;
		}

		status = inContentContext->FinalizeCurrentStream();

	}while(false);

	return status;

}

static const string scParent = "Parent";
PDFObject* PDFDocumentHandler::QueryInheritedValue(PDFDictionary* inDictionary,string inName)
{
	if(inDictionary->Exists(inName))
	{
		return mParser.QueryDictionaryObject(inDictionary,inName);
	}
	else if(inDictionary->Exists(scParent))
	{
		PDFObjectCastPtr<PDFDictionary> parent(mParser.QueryDictionaryObject(inDictionary,scParent));
		if(!parent)
			return NULL;
		return QueryInheritedValue(parent.GetPtr(),inName);
	}
	else
		return NULL;
}
