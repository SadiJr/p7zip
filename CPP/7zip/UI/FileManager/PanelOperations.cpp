// PanelOperations.cpp

#include "StdAfx.h"

#include "../../../Common/DynamicBuffer.h"
#include "../../../Common/StringConvert.h"
#include "../../../Common/Wildcard.h"

#include "../../../Windows/COM.h"
#include "../../../Windows/FileName.h"
#include "../../../Windows/PropVariant.h"

#include "ComboDialog.h"

#include "FSFolder.h"
#include "FormatUtils.h"
#include "LangUtils.h"
#include "Panel.h"
#include "UpdateCallback100.h"

#include "resource.h"

using namespace NWindows;
using namespace NFile;
using namespace NName;

#ifndef _UNICODE
extern bool g_IsNT;
#endif

enum EFolderOpType
{
  FOLDER_TYPE_CREATE_FOLDER = 0,
  FOLDER_TYPE_DELETE = 1,
  FOLDER_TYPE_RENAME = 2
};

class CThreadFolderOperations: public CProgressThreadVirt
{
  HRESULT ProcessVirt();
public:
  EFolderOpType OpType;
  UString Name;
  UInt32 Index;
  CRecordVector<UInt32> Indices;

  CMyComPtr<IFolderOperations> FolderOperations;
  CMyComPtr<IProgress> UpdateCallback;
  CUpdateCallback100Imp *UpdateCallbackSpec;
  
  HRESULT Result;

  CThreadFolderOperations(EFolderOpType opType): OpType(opType), Result(E_FAIL) {}
  HRESULT DoOperation(CPanel &panel, const UString &progressTitle, const UString &titleError);
};
  
HRESULT CThreadFolderOperations::ProcessVirt()
{
  // FIXME NCOM::CComInitializer comInitializer;
  switch(OpType)
  {
    case FOLDER_TYPE_CREATE_FOLDER:
      Result = FolderOperations->CreateFolder(Name, UpdateCallback);
      break;
    case FOLDER_TYPE_DELETE:
      Result = FolderOperations->Delete(&Indices.Front(), Indices.Size(), UpdateCallback);
      break;
    case FOLDER_TYPE_RENAME:
      Result = FolderOperations->Rename(Index, Name, UpdateCallback);
      break;
    default:
      Result = E_FAIL;
  }
  return Result;
}


HRESULT CThreadFolderOperations::DoOperation(CPanel &panel, const UString &progressTitle, const UString &titleError)
{
  UpdateCallbackSpec = new CUpdateCallback100Imp;
  UpdateCallback = UpdateCallbackSpec;
  UpdateCallbackSpec->ProgressDialog = &ProgressDialog;

  // FIXME ProgressDialog.WaitMode = true;
  ProgressDialog.Sync.FinalMessage.ErrorMessage.Title = titleError;
  Result = S_OK;

  bool usePassword = false;
  UString password;
  if (panel._parentFolders.Size() > 0)
  {
    const CFolderLink &fl = panel._parentFolders.Back();
    usePassword = fl.UsePassword;
    password = fl.Password;
  }

  UpdateCallbackSpec->Init(usePassword, password);

  ProgressDialog.MainWindow = panel._mainWindow; // panel.GetParent()
  ProgressDialog.MainTitle = L"7-Zip"; // LangString(IDS_APP_TITLE);
  ProgressDialog.MainAddTitle = progressTitle + L' ';

  RINOK(Create(progressTitle, ProgressDialog.MainWindow));
  return Result;
}

#ifndef _UNICODE
typedef int (WINAPI * SHFileOperationWP)(LPSHFILEOPSTRUCTW lpFileOp);
#endif

void CPanel::MessageBoxErrorForUpdate(HRESULT errorCode, UINT resourceID)
{
  if (errorCode == E_NOINTERFACE)
    MessageBoxErrorLang(IDS_OPERATION_IS_NOT_SUPPORTED);
  else
    MessageBoxError(errorCode, LangString(resourceID));
}

void CPanel::DeleteItems(bool NON_CE_VAR(toRecycleBin))
{
  CDisableTimerProcessing disableTimerProcessing(*this);
  CRecordVector<UInt32> indices;
  GetOperatedItemIndices(indices);
  if (indices.IsEmpty())
    return;
  CSelectedState state;
  SaveSelectedState(state);

  #ifndef UNDER_CE
  // WM6 / SHFileOperationW doesn't ask user! So we use internal delete
  if (IsFSFolder() && toRecycleBin)
  {
    bool useInternalDelete = false;
    #ifndef _UNICODE
    if (!g_IsNT)
    {
      CDynamicBuffer<CHAR> buffer;
      FOR_VECTOR (i, indices)
      {
        const AString path = GetSystemString(GetFsPath() + GetItemRelPath(indices[i]));
        memcpy(buffer.GetCurPtrAndGrow(path.Len() + 1), (const CHAR *)path, (path.Len() + 1) * sizeof(CHAR));
      }
      *buffer.GetCurPtrAndGrow(1) = 0;
      SHFILEOPSTRUCTA fo;
      fo.hwnd = GetParent();
      fo.wFunc = FO_DELETE;
      fo.pFrom = (const CHAR *)buffer;
      fo.pTo = 0;
      fo.fFlags = 0;
      if (toRecycleBin)
        fo.fFlags |= FOF_ALLOWUNDO;
      // fo.fFlags |= FOF_NOCONFIRMATION;
      // fo.fFlags |= FOF_NOERRORUI;
      // fo.fFlags |= FOF_SILENT;
      // fo.fFlags |= FOF_WANTNUKEWARNING;
      fo.fAnyOperationsAborted = FALSE;
      fo.hNameMappings = 0;
      fo.lpszProgressTitle = 0;
      /* int res = */ ::SHFileOperationA(&fo);
    }
    else
    #endif
    {
      CDynamicBuffer<WCHAR> buffer;
      unsigned maxLen = 0;
      FOR_VECTOR (i, indices)
      {
        // L"\\\\?\\") doesn't work here.
        const UString path = GetFsPath() + GetItemRelPath(indices[i]);
        if (path.Len() > maxLen)
          maxLen = path.Len();
        memcpy(buffer.GetCurPtrAndGrow(path.Len() + 1), (const WCHAR *)path, (path.Len() + 1) * sizeof(WCHAR));
      }
      *buffer.GetCurPtrAndGrow(1) = 0;
#ifdef _WIN32
      if (maxLen >= MAX_PATH)
      {
        if (toRecycleBin)
        {
          MessageBoxErrorLang(IDS_ERROR_LONG_PATH_TO_RECYCLE);
          return;
        }
        useInternalDelete = true;
      }
      else
      {
        SHFILEOPSTRUCTW fo;
        fo.hwnd = GetParent();
        fo.wFunc = FO_DELETE;
        fo.pFrom = (const WCHAR *)buffer;
        fo.pTo = 0;
        fo.fFlags = 0;
        if (toRecycleBin)
          fo.fFlags |= FOF_ALLOWUNDO;
        fo.fAnyOperationsAborted = FALSE;
        fo.hNameMappings = 0;
        fo.lpszProgressTitle = 0;
        int res;
        #ifdef _UNICODE
        res = ::SHFileOperationW(&fo);
        #else
        SHFileOperationWP shFileOperationW = (SHFileOperationWP)
          ::GetProcAddress(::GetModuleHandleW(L"shell32.dll"), "SHFileOperationW");
        if (shFileOperationW == 0)
          return;
        res = shFileOperationW(&fo);
        #endif
      }
#else
      // FIXME - how to use the recycle bin undex Gnome or KDE ?
      useInternalDelete = true;
#endif
    }
    /*
    if (fo.fAnyOperationsAborted)
      MessageBoxError(result, LangString(IDS_ERROR_DELETING, 0x03020217));
    */
    if (!useInternalDelete)
    {
      RefreshListCtrl(state);
      return;
    }
  }
  #endif
 
  // DeleteItemsInternal

  CMyComPtr<IFolderOperations> folderOperations;
  if (_folder.QueryInterface(IID_IFolderOperations, &folderOperations) != S_OK)
  {
    MessageBoxErrorForUpdate(E_NOINTERFACE, IDS_ERROR_DELETING);
    return;
  }

  UInt32 titleID, messageID;
  UString messageParam;
  if (indices.Size() == 1)
  {
    int index = indices[0];
    messageParam = GetItemRelPath(index);
    if (IsItem_Folder(index))
    {
      titleID = IDS_CONFIRM_FOLDER_DELETE;
      messageID = IDS_WANT_TO_DELETE_FOLDER;
    }
    else
    {
      titleID = IDS_CONFIRM_FILE_DELETE;
      messageID = IDS_WANT_TO_DELETE_FILE;
    }
  }
  else
  {
    titleID = IDS_CONFIRM_ITEMS_DELETE;
    messageID = IDS_WANT_TO_DELETE_ITEMS;
    messageParam = NumberToString(indices.Size());
  }
  if (::MessageBoxW(GetParent(), MyFormatNew(messageID, messageParam), LangString(titleID), MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
    return;

  CDisableNotify disableNotify(*this);
  {
    CThreadFolderOperations op(FOLDER_TYPE_DELETE);
    op.FolderOperations = folderOperations;
    op.Indices = indices;
    op.DoOperation(*this,
        LangString(IDS_DELETING),
        LangString(IDS_ERROR_DELETING));
  }
  RefreshTitleAlways();
  RefreshListCtrl(state);
}

#ifdef _WIN32
BOOL CPanel::OnBeginLabelEdit(LV_DISPINFOW * lpnmh)
{
  int realIndex = GetRealIndex(lpnmh->item);
  if (realIndex == kParentIndex)
    return TRUE;
  CMyComPtr<IFolderOperations> folderOperations;
  if (_folder.QueryInterface(IID_IFolderOperations, &folderOperations) != S_OK)
    return TRUE;
  return FALSE;
}
#endif

static UString GetLastPart(const UString name)
{
  int slashPos = name.ReverseFind(L'/');
  #ifdef _WIN32
  int slash1Pos = name.ReverseFind(L'\\');
  slashPos = MyMax(slashPos, slash1Pos);
  #endif
  return name.Ptr(slashPos + 1);
}

bool IsCorrectFsName(const UString &name)
{
  const UString lastPart = GetLastPart(name);
  return
      lastPart != L"." &&
      lastPart != L"..";
}

bool CorrectFsPath(const UString &relBase, const UString &path, UString &result);

bool CPanel::CorrectFsPath(const UString &path2, UString &result)
{
  return ::CorrectFsPath(_currentFolderPrefix, path2, result);
}

#ifdef _WIN32
BOOL CPanel::OnEndLabelEdit(LV_DISPINFOW * lpnmh)
{
  if (lpnmh->item.pszText == NULL)
    return FALSE;
  CDisableTimerProcessing disableTimerProcessing2(*this);
  CMyComPtr<IFolderOperations> folderOperations;
  if (_folder.QueryInterface(IID_IFolderOperations, &folderOperations) != S_OK)
  {
    MessageBoxErrorForUpdate(E_NOINTERFACE, IDS_ERROR_RENAMING);
    return FALSE;
  }
  UString newName = lpnmh->item.pszText;
  if (!IsCorrectFsName(newName))
  {
    MessageBoxError(E_INVALIDARG);
    return FALSE;
  }

  if (IsFSFolder())
  {
    UString correctName;
    if (!CorrectFsPath(newName, correctName))
    {
      MessageBoxError(E_INVALIDARG);
      return FALSE;
    }
    newName = correctName;
  }

  SaveSelectedState(_selectedState);

  int realIndex = GetRealIndex(lpnmh->item);
  if (realIndex == kParentIndex)
    return FALSE;
  const UString prefix = GetItemPrefix(realIndex);


  CDisableNotify disableNotify(*this);
  {
    CThreadFolderOperations op(FOLDER_TYPE_RENAME);
    op.FolderOperations = folderOperations;
    op.Index = realIndex;
    op.Name = newName;
    /* HRESULTres = */ op.DoOperation(*this,
        LangString(IDS_RENAMING),
        LangString(IDS_ERROR_RENAMING));
    // fixed in 9.26: we refresh list even after errors
    // (it's more safe, since error can be at different stages, so list can be incorrect).
    /*
    if (res != S_OK)
      return FALSE;
    */
  }

  // Can't use RefreshListCtrl here.
  // RefreshListCtrlSaveFocused();
  _selectedState.FocusedName = prefix + newName;
  _selectedState.SelectFocused = true;

  // We need clear all items to disable GetText before Reload:
  // number of items can change.
  // _listView.DeleteAllItems();
  // But seems it can still call GetText (maybe for current item)
  // so we can't delete items.

  _dontShowMode = true;

  PostMessage(kReLoadMessage);
  return TRUE;
}
#endif

bool Dlg_CreateFolder(HWND wnd, UString &destName);

void CPanel::CreateFolder()
{
  CMyComPtr<IFolderOperations> folderOperations;
  if (_folder.QueryInterface(IID_IFolderOperations, &folderOperations) != S_OK)
  {
    MessageBoxErrorForUpdate(E_NOINTERFACE, IDS_CREATE_FOLDER_ERROR);
    return;
  }
  CDisableTimerProcessing disableTimerProcessing2(*this);
  CSelectedState state;
  SaveSelectedState(state);

  UString newName;
  if (!Dlg_CreateFolder(GetParent(), newName))
    return;
  
  if (!IsCorrectFsName(newName))
  {
    MessageBoxError(E_INVALIDARG);
    return;
  }

  if (IsFSFolder())
  {
    UString correctName;
    if (!CorrectFsPath(newName, correctName))
    {
      MessageBoxError(E_INVALIDARG);
      return;
    }
    newName = correctName;
  }
  
  HRESULT res;
  CDisableNotify disableNotify(*this);
  {
    CThreadFolderOperations op(FOLDER_TYPE_CREATE_FOLDER);
    op.FolderOperations = folderOperations;
    op.Name = newName;
    res = op.DoOperation(*this,
        LangString(IDS_CREATE_FOLDER),
        LangString(IDS_CREATE_FOLDER_ERROR));
    /*
    // fixed for 9.26: we must refresh always
    if (res != S_OK)
      return;
    */
  }
  if (res == S_OK)
  {
    int pos = newName.Find(WCHAR_PATH_SEPARATOR);
    if (pos >= 0)
      newName.DeleteFrom(pos);
    if (!_mySelectMode)
      state.SelectedNames.Clear();
    state.FocusedName = newName;
    state.SelectFocused = true;
  }
  RefreshTitleAlways();
  RefreshListCtrl(state);
}

void CPanel::CreateFile()
{
  CMyComPtr<IFolderOperations> folderOperations;
  if (_folder.QueryInterface(IID_IFolderOperations, &folderOperations) != S_OK)
  {
    MessageBoxErrorForUpdate(E_NOINTERFACE, IDS_CREATE_FILE_ERROR);
    return;
  }
  CDisableTimerProcessing disableTimerProcessing2(*this);
  CSelectedState state;
  SaveSelectedState(state);
  CComboDialog dlg;
  LangString(IDS_CREATE_FILE, dlg.Title);
  LangString(IDS_CREATE_FILE_NAME, dlg.Static);
  LangString(IDS_CREATE_FILE_DEFAULT_NAME, dlg.Value);

  if (dlg.Create(GetParent()) != IDOK)
    return;

  CDisableNotify disableNotify(*this);
  
  UString newName = dlg.Value;

  if (IsFSFolder())
  {
    UString correctName;
    if (!CorrectFsPath(newName, correctName))
    {
      MessageBoxError(E_INVALIDARG);
      return;
    }
    newName = correctName;
  }

  HRESULT result = folderOperations->CreateFile(newName, 0);
  if (result != S_OK)
  {
    MessageBoxErrorForUpdate(result, IDS_CREATE_FILE_ERROR);
    return;
  }
  int pos = newName.Find(WCHAR_PATH_SEPARATOR);
  if (pos >= 0)
    newName.DeleteFrom(pos);
  if (!_mySelectMode)
    state.SelectedNames.Clear();
  state.FocusedName = newName;
  state.SelectFocused = true;
  RefreshListCtrl(state);
}

void CPanel::RenameFile()
{
  int index = _listView.GetFocusedItem();
  if (index >= 0)
    _listView.EditLabel(index);
}

void CPanel::ChangeComment()
{
  CDisableTimerProcessing disableTimerProcessing2(*this);
  int index = _listView.GetFocusedItem();
  if (index < 0)
    return;
  int realIndex = GetRealItemIndex(index);
  if (realIndex == kParentIndex)
    return;
  CSelectedState state;
  SaveSelectedState(state);
  CMyComPtr<IFolderOperations> folderOperations;
  if (_folder.QueryInterface(IID_IFolderOperations, &folderOperations) != S_OK)
  {
    MessageBoxErrorLang(IDS_OPERATION_IS_NOT_SUPPORTED);
    return;
  }

  UString comment;
  {
    NCOM::CPropVariant propVariant;
    if (_folder->GetProperty(realIndex, kpidComment, &propVariant) != S_OK)
      return;
    if (propVariant.vt == VT_BSTR)
      comment = propVariant.bstrVal;
    else if (propVariant.vt != VT_EMPTY)
      return;
  }
  UString name = GetItemRelPath(realIndex);
  CComboDialog dlg;
  dlg.Title = name + L' ' + LangString(IDS_COMMENT);
  dlg.Value = comment;
  LangString(IDS_COMMENT2, dlg.Static);
  if (dlg.Create(GetParent()) != IDOK)
    return;
  NCOM::CPropVariant propVariant(dlg.Value);

  CDisableNotify disableNotify(*this);
  HRESULT result = folderOperations->SetProperty(realIndex, kpidComment, &propVariant, NULL);
  if (result != S_OK)
  {
    if (result == E_NOINTERFACE)
      MessageBoxErrorLang(IDS_OPERATION_IS_NOT_SUPPORTED);
    else
      MessageBoxError(result, L"Set Comment Error");
  }
  RefreshListCtrl(state);
}

// From CPP/7zip/UI/FileManager/BrowseDialog.cpp
bool Dlg_CreateFolder(HWND wnd, UString &destName)
{
  destName.Empty();
  CComboDialog dlg;
  LangString(IDS_CREATE_FOLDER, dlg.Title);
  LangString(IDS_CREATE_FOLDER_NAME, dlg.Static);
  LangString(IDS_CREATE_FOLDER_DEFAULT_NAME, dlg.Value);
  if (dlg.Create(wnd) != IDOK)
    return false;
  destName = dlg.Value;
  return true;
}

