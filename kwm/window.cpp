#include "kwm.h"

static CGWindowListOption OsxWindowListOption = kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements;

extern screen_info *Screen;
extern int MarkedWindowID;

extern std::vector<window_info> WindowLst;
extern std::vector<int> FloatingSpaceLst;
extern std::vector<std::string> FloatingAppLst;
extern std::vector<int> FloatingWindowLst;

extern ProcessSerialNumber FocusedPSN;
extern window_info *FocusedWindow;
extern focus_option KwmFocusMode;
extern int KwmSplitMode;
extern bool KwmUseBSPTilingMode;
extern bool KwmUseContextMenuFix;

int OldScreenID = 0;
int PrevSpace = -1, CurrentSpace = 0;
bool ForceRefreshFocus = false;
bool IsContextualMenusVisible = false;
window_info FocusedWindowCache;
CFStringRef DisplayIdentifier;

std::map<int, window_role> WindowRoleCache;
std::map<int, std::vector<AXUIElementRef> > WindowRefsCache;

bool WindowsAreEqual(window_info *Window, window_info *Match)
{
    bool Result = false;

    if(!ForceRefreshFocus && Window && Match)
    {
        if(Window->PID == Match->PID &&
           Window->WID == Match->WID &&
           Window->Layer == Match->Layer)
        {
            Result = true;
        }
    }

    return Result;
}

bool IsWindowNotAStandardWindow(window_info *Window)
{
    bool Result = false;

    // A non standard window is a window that
    // doesnot have a subrole equal to
    // kAXStandardWindowSubrole
    // This is usually windows that do not have
    // a title-bar.
    if(Window->Owner == "iTerm2")
    {
        Result = true;
    }

    return Result;
}

bool IsContextMenusAndSimilarVisible()
{
    bool Result = false;

    for(int WindowIndex = 0; WindowIndex < WindowLst.size(); ++WindowIndex)
    {
        if((WindowLst[WindowIndex].Owner != "Dock" ||
            WindowLst[WindowIndex].Name != "Dock") &&
            WindowLst[WindowIndex].Layer != 0)
        {
            Result = true;
            break;
        }
    }

    return Result;
}

bool FilterWindowList(screen_info *Screen)
{
    bool Result = true;
    std::vector<window_info> FilteredWindowLst;

    for(int WindowIndex = 0; WindowIndex < WindowLst.size(); ++WindowIndex)
    {
        // Mission-Control mode is on and so we do not try to tile windows
        if(WindowLst[WindowIndex].Owner == "Dock" &&
           WindowLst[WindowIndex].Name == "")
               Result = false;

        if(KwmUseContextMenuFix)
            IsContextualMenusVisible = IsContextMenusAndSimilarVisible();

        if(WindowLst[WindowIndex].Layer == 0 &&
           Screen == GetDisplayOfWindow(&WindowLst[WindowIndex]))
        {
            CFTypeRef Role, SubRole;
            if(GetWindowRole(&WindowLst[WindowIndex], &Role, &SubRole))
            {
                if(CFEqual(Role, kAXWindowRole))
                {
                    if(!IsWindowNotAStandardWindow(&WindowLst[WindowIndex]))
                    {
                        if(CFEqual(SubRole, kAXStandardWindowSubrole))
                            FilteredWindowLst.push_back(WindowLst[WindowIndex]);
                    }
                    else
                    {
                        FilteredWindowLst.push_back(WindowLst[WindowIndex]);
                    }
                }
            }
        }
    }

    WindowLst = FilteredWindowLst;
    return Result;
}

bool IsCursorInsideFocusedWindow()
{
    bool Result = false;

    if(Screen && FocusedWindow)
    {
        space_info *Space = &Screen->Space[Screen->ActiveSpace];
        tree_node *Node = GetNodeFromWindowID(Space->RootNode, FocusedWindow->WID);
        if(Node)
        {
            CGPoint Cursor = GetCursorPos();
            if((Cursor.x >= Node->Container.X) &&
                (Cursor.x <= Node->Container.X + Node->Container.Width) &&
                (Cursor.y >= Node->Container.Y) &&
                (Cursor.y <= Node->Container.Y + Node->Container.Height))
                    Result = true;
        }
    }

    return Result;
}

bool IsSpaceFloating(int SpaceID, int *Index)
{
    bool Result = false;

    for(int SpaceIndex = 0; SpaceIndex < FloatingSpaceLst.size(); ++SpaceIndex)
    {
        if(SpaceID == FloatingSpaceLst[SpaceIndex])
        {
            Result = true;

            if(Index)
                *Index = SpaceIndex;

            break;
        }
    }

    return Result;
}

bool IsApplicationFloating(window_info *Window)
{
    bool Result = false;

    for(int WindowIndex = 0; WindowIndex < FloatingAppLst.size(); ++WindowIndex)
    {
        if(Window->Owner == FloatingAppLst[WindowIndex])
        {
            Result = true;
            break;
        }
    }

    return Result;
}

bool IsWindowFloating(int WindowID, int *Index)
{
    bool Result = false;

    for(int WindowIndex = 0; WindowIndex < FloatingWindowLst.size(); ++WindowIndex)
    {
        if(WindowID == FloatingWindowLst[WindowIndex])
        {
            DEBUG("IsWindowFloating(): floating " << WindowID)
            Result = true;

            if(Index)
                *Index = WindowIndex;

            break;
        }
    }

    return Result;
}

bool IsWindowBelowCursor(window_info *Window)
{
    bool Result = false;

    if(Window)
    {
        CGPoint Cursor = GetCursorPos();
        if(Cursor.x >= Window->X && 
           Cursor.x <= Window->X + Window->Width &&
           Cursor.y >= Window->Y &&
           Cursor.y <= Window->Y + Window->Height)
        {
            Result = true;
        }
    }
        
    return Result;
}

bool DoesSpaceExistInMapOfScreen(screen_info *Screen)
{
    std::map<int, space_info>::iterator It = Screen->Space.find(Screen->ActiveSpace);
    if(It == Screen->Space.end())
        return false;
    else
        return It->second.RootNode != NULL;
}

bool IsWindowOnActiveSpace(window_info *Window)
{
    for(int WindowIndex = 0; WindowIndex < WindowLst.size(); ++WindowIndex)
    {
        if(WindowsAreEqual(Window, &WindowLst[WindowIndex]))
        {
            DEBUG("IsWindowOnActiveSpace() window found")
            return true;
        }
    }

    DEBUG("IsWindowOnActiveSpace() window was not found")
    return false;
}

bool IsSpaceTransitionInProgress()
{
    bool Result = CGSManagedDisplayIsAnimating(CGSDefaultConnection, (CFStringRef)DisplayIdentifier);
    if(Result)
    {
        DEBUG("IsSpaceTransitionInProgress() Space transition detected")
    }

    return Result;
}

bool IsSpaceSystemOrFullscreen()
{
    bool Result = CGSSpaceGetType(CGSDefaultConnection, Screen->ActiveSpace) != CGSSpaceTypeUser;
    if(Result)
    {
        DEBUG("IsSpaceSystemOrFullscreen() Space is not user created")
    }

    return Result;
}

void FocusWindowBelowCursor()
{
    if(IsSpaceTransitionInProgress() ||
       IsSpaceSystemOrFullscreen() ||
       (KwmUseContextMenuFix && IsContextualMenusVisible))
           return;

    for(int WindowIndex = 0; WindowIndex < WindowLst.size(); ++WindowIndex)
    {
        if(IsWindowBelowCursor(&WindowLst[WindowIndex]))
        {
            if(!WindowsAreEqual(FocusedWindow, &WindowLst[WindowIndex]))
            {
                // Note: Memory leak related to this function-call
                SetWindowFocus(&WindowLst[WindowIndex]);
                DEBUG("FocusWindowBelowCursor() Current space: " << Screen->ActiveSpace)
            }
            break;
        }
    }
}

void UpdateWindowTree()
{
    OldScreenID = Screen->ID;
    Screen = GetDisplayOfMousePointer();
    if(!Screen)
        return;

    UpdateActiveWindowList(Screen);

    if(KwmUseBSPTilingMode &&
       !IsSpaceTransitionInProgress() &&
       !IsSpaceSystemOrFullscreen() &&
       FilterWindowList(Screen))
    {
        std::vector<window_info*> WindowsOnDisplay = GetAllWindowsOnDisplay(Screen->ID);
        std::map<int, space_info>::iterator It = Screen->Space.find(Screen->ActiveSpace);
        if(!IsSpaceFloating(Screen->ActiveSpace, NULL))
        {
            if(It == Screen->Space.end() && !WindowsOnDisplay.empty())
                CreateWindowNodeTree(Screen, &WindowsOnDisplay);
            else if(It != Screen->Space.end() && !WindowsOnDisplay.empty())
                ShouldWindowNodeTreeUpdate(Screen);
            else if(It != Screen->Space.end() && WindowsOnDisplay.empty())
                Screen->Space.erase(Screen->ActiveSpace);
        }
    }
}

void UpdateActiveWindowList(screen_info *Screen)
{
    Screen->OldWindowListCount = WindowLst.size();
    WindowLst.clear();

    CFArrayRef OsxWindowLst = CGWindowListCopyWindowInfo(OsxWindowListOption, kCGNullWindowID);
    if(!OsxWindowLst)
        return;

    CFIndex OsxWindowCount = CFArrayGetCount(OsxWindowLst);
    for(CFIndex WindowIndex = 0; WindowIndex < OsxWindowCount; ++WindowIndex)
    {
        CFDictionaryRef Elem = (CFDictionaryRef)CFArrayGetValueAtIndex(OsxWindowLst, WindowIndex);
        WindowLst.push_back(window_info());
        CFDictionaryApplyFunction(Elem, GetWindowInfo, NULL);
    }
    CFRelease(OsxWindowLst);

    ForceRefreshFocus = true;
    PrevSpace = CurrentSpace;
    if(OldScreenID != Screen->ID)
    {
        if(Screen->ActiveSpace == 0)
        {
            do
            {
                CurrentSpace = CGSGetActiveSpace(CGSDefaultConnection);
                usleep(200000);
            } while(PrevSpace == CurrentSpace);
            Screen->ActiveSpace = CurrentSpace;
        }
        else
        {
            CurrentSpace = Screen->ActiveSpace;
        }

        if(DisplayIdentifier)
            CFRelease(DisplayIdentifier);

        DisplayIdentifier = CGSCopyManagedDisplayForSpace(CGSDefaultConnection, Screen->ActiveSpace);

        if(Screen->ForceContainerUpdate)
        {
            ApplyNodeContainer(Screen->Space[Screen->ActiveSpace].RootNode);
            Screen->ForceContainerUpdate = false;
        }

        DEBUG("UpdateActiveWindowList() Active Display Changed")
        FocusWindowBelowCursor();
    }
    else
    {
        CurrentSpace = CGSGetActiveSpace(CGSDefaultConnection);
        if(PrevSpace != CurrentSpace)
        {
            DEBUG("UpdateActiveWindowList() Space transition ended")
            if(DisplayIdentifier)
                CFRelease(DisplayIdentifier);

            Screen->ActiveSpace = CurrentSpace;
            DisplayIdentifier = CGSCopyManagedDisplayForSpace(CGSDefaultConnection, Screen->ActiveSpace);
            FocusWindowBelowCursor();
        }
    }

    ForceRefreshFocus = false;
}

void CreateWindowNodeTree(screen_info *Screen, std::vector<window_info*> *Windows)
{
    DEBUG("CreateWindowNodeTree() Create Tree")

    space_info SpaceInfo;
    SpaceInfo.RootNode = CreateTreeFromWindowIDList(Screen, Windows);

    if(SpaceInfo.RootNode)
    {
        SpaceInfo.PaddingTop = Screen->PaddingTop;
        SpaceInfo.PaddingBottom = Screen->PaddingBottom;
        SpaceInfo.PaddingLeft = Screen->PaddingLeft;
        SpaceInfo.PaddingRight = Screen->PaddingRight;

        SpaceInfo.VerticalGap = Screen->VerticalGap;
        SpaceInfo.HorizontalGap = Screen->HorizontalGap;

        Screen->Space[Screen->ActiveSpace] = SpaceInfo;
        ApplyNodeContainer(Screen->Space[Screen->ActiveSpace].RootNode);
        FocusWindowBelowCursor();
    }
    else
    {
        Screen->Space.erase(Screen->ActiveSpace);
    }
}

void ShouldWindowNodeTreeUpdate(screen_info *Screen)
{
    if(Screen->ActiveSpace == -1 || PrevSpace != Screen->ActiveSpace || Screen->OldWindowListCount == -1)
        return;

    space_info *Space = &Screen->Space[Screen->ActiveSpace];
    if(WindowLst.size() > Screen->OldWindowListCount)
    {
        DEBUG("ShouldWindowNodeTreeUpdate() Add Window")
        for(int WindowIndex = 0; WindowIndex < WindowLst.size(); ++WindowIndex)
        {
            if(GetNodeFromWindowID(Space->RootNode, WindowLst[WindowIndex].WID) == NULL)
            {
                if(!IsApplicationFloating(&WindowLst[WindowIndex]) &&
                        !IsWindowFloating(WindowLst[WindowIndex].WID, NULL))
                {
                    AddWindowToTree(Screen, WindowLst[WindowIndex].WID);
                    SetWindowFocus(&WindowLst[WindowIndex]);
                }
            }
        }
    }
    else if(WindowLst.size() < Screen->OldWindowListCount)
    {
        DEBUG("ShouldWindowNodeTreeUpdate() Remove Window")
        std::vector<int> WindowIDsInTree;

        tree_node *CurrentNode = Space->RootNode;
        while(CurrentNode->LeftChild)
            CurrentNode = CurrentNode->LeftChild;

        while(CurrentNode)
        {
            WindowIDsInTree.push_back(CurrentNode->WindowID);
            CurrentNode = GetNearestNodeToTheRight(CurrentNode);
        }

        for(int IDIndex = 0; IDIndex < WindowIDsInTree.size(); ++IDIndex)
        {
            bool Found = false;
            for(int WindowIndex = 0; WindowIndex < WindowLst.size(); ++WindowIndex)
            {
                if(WindowLst[WindowIndex].WID == WindowIDsInTree[IDIndex])
                {
                    Found = true;
                    break;
                }
            }

            if(!Found)
                RemoveWindowFromTree(Screen, WindowIDsInTree[IDIndex], false);
        }
    }
}

void AddWindowToTree(screen_info *Screen, int WindowID)
{
    if(!Screen || !DoesSpaceExistInMapOfScreen(Screen))
        return;

    space_info *Space = &Screen->Space[Screen->ActiveSpace];
    tree_node *RootNode = Space->RootNode;
    tree_node *CurrentNode = RootNode;

    DEBUG("AddWindowToTree() Create pair of leafs")
    bool UseFocusedContainer = FocusedWindow &&
                               IsWindowOnActiveSpace(FocusedWindow) &&
                               FocusedWindow->WID != WindowID;

    bool DoNotUseMarkedContainer = IsWindowFloating(MarkedWindowID, NULL) ||
                                   (MarkedWindowID == WindowID);

    if(MarkedWindowID == -1 && UseFocusedContainer)
    {
        CurrentNode = GetNodeFromWindowID(RootNode, FocusedWindow->WID);
    }
    else if(DoNotUseMarkedContainer || (MarkedWindowID == -1 && !UseFocusedContainer))
    {
        while(!IsLeafNode(CurrentNode))
        {
            if(!IsLeafNode(CurrentNode->LeftChild) && IsLeafNode(CurrentNode->RightChild))
                CurrentNode = CurrentNode->RightChild;
            else
                CurrentNode = CurrentNode->LeftChild;
        }
    }
    else
    {
        CurrentNode = GetNodeFromWindowID(RootNode, MarkedWindowID);
        MarkedWindowID = -1;
    }

    if(CurrentNode)
    {
        int SplitMode = KwmSplitMode == -1 ? GetOptimalSplitMode(CurrentNode) : KwmSplitMode;
        CreateLeafNodePair(Screen, CurrentNode, CurrentNode->WindowID, WindowID, SplitMode);
        ApplyNodeContainer(CurrentNode);
    }
}

void AddWindowToTree()
{
    if(!Screen)
        return;

    AddWindowToTree(Screen, FocusedWindow->WID);
}

void RemoveWindowFromTree(screen_info *Screen, int WindowID, bool Center)
{
    if(!DoesSpaceExistInMapOfScreen(Screen))
        return;

    space_info *Space = &Screen->Space[Screen->ActiveSpace];
    tree_node *WindowNode = GetNodeFromWindowID(Space->RootNode, WindowID);
    if(!WindowNode)
        return;

    tree_node *Parent = WindowNode->Parent;
    if(Parent && Parent->LeftChild && Parent->RightChild)
    {
        tree_node *OldLeftChild = Parent->LeftChild;
        tree_node *OldRightChild = Parent->RightChild;
        tree_node *AccessChild;

        Parent->LeftChild = NULL;
        Parent->RightChild = NULL;

        if(OldRightChild == WindowNode)
        {
            if(OldLeftChild)
                AccessChild = OldLeftChild;
        }
        else
        {
            if(OldRightChild)
                AccessChild = OldRightChild;
        }

        if(AccessChild)
        {
            DEBUG("RemoveWindowFromTree() " << FocusedWindow->Name)
            Parent->WindowID = AccessChild->WindowID;
            if(AccessChild->LeftChild && AccessChild->RightChild)
            {
                Parent->LeftChild = AccessChild->LeftChild;
                Parent->LeftChild->Parent = Parent;

                Parent->RightChild = AccessChild->RightChild;
                Parent->RightChild->Parent = Parent;

                CreateNodeContainers(Screen, Parent, true);
            }

            free(AccessChild);
            free(WindowNode);
            ApplyNodeContainer(Parent);

            if(Center)
                CenterWindow(Screen);
            else
                FocusWindowBelowCursor();
        }
    }
    else if(!Parent)
    {
        DEBUG("RemoveWindowFromTree() " << FocusedWindow->Name)

        free(WindowNode);
        Screen->Space[Screen->ActiveSpace].RootNode = NULL;
        if(Center)
            CenterWindow(Screen);

        Screen->Space.erase(Screen->ActiveSpace);
    }
}

void RemoveWindowFromTree()
{
    if(!Screen)
        return;

    RemoveWindowFromTree(Screen, FocusedWindow->WID, true);
}

void AddWindowToTreeOfUnfocusedMonitor(screen_info *Screen)
{
    if(!Screen)
        return;

    space_info *Space = &Screen->Space[Screen->ActiveSpace];
    if(Space->RootNode)
    {
        tree_node *RootNode = Space->RootNode;
        tree_node *CurrentNode = RootNode;

        DEBUG("AddWindowToTreeOfUnfocusedMonitor() Create pair of leafs")

        while(!IsLeafNode(CurrentNode))
        {
            if(!IsLeafNode(CurrentNode->LeftChild) && IsLeafNode(CurrentNode->RightChild))
                CurrentNode = CurrentNode->RightChild;
            else
                CurrentNode = CurrentNode->LeftChild;
        }

        int SplitMode = KwmSplitMode == -1 ? GetOptimalSplitMode(CurrentNode) : KwmSplitMode;
        CreateLeafNodePair(Screen, CurrentNode, CurrentNode->WindowID, FocusedWindow->WID, SplitMode);
        ResizeWindowToContainerSize(CurrentNode->RightChild);
        Screen->ForceContainerUpdate = true;
    }
    else if(Screen->ActiveSpace != 0)
    {
        std::vector<window_info*> WindowsOnDisplay;
        WindowsOnDisplay.push_back(FocusedWindow);
        CreateWindowNodeTree(Screen, &WindowsOnDisplay);
    }
}

void FloatFocusedSpace()
{
    if(Screen && Screen->ActiveSpace != 0)
    {
        if(!IsSpaceFloating(Screen->ActiveSpace, NULL))
        {
            DestroyNodeTree(Screen->Space[Screen->ActiveSpace].RootNode);
            Screen->Space.erase(Screen->ActiveSpace);
            FloatingSpaceLst.push_back(Screen->ActiveSpace);
        }
    }
}

void TileFocusedSpace()
{
    if(Screen && Screen->ActiveSpace != 0)
    {
        int SpaceIndex;
        if(IsSpaceFloating(Screen->ActiveSpace, &SpaceIndex))
            FloatingSpaceLst.erase(FloatingSpaceLst.begin() + SpaceIndex);
    }
}

void ToggleFocusedSpaceFloating()
{
    if(Screen && Screen->ActiveSpace != 0)
    {
        if(IsSpaceFloating(Screen->ActiveSpace, NULL))
            TileFocusedSpace();
        else
            FloatFocusedSpace();
    }
}

void ToggleFocusedWindowFloating()
{
    if(FocusedWindow && IsWindowOnActiveSpace(FocusedWindow))
    {
        int WindowIndex;
        if(IsWindowFloating(FocusedWindow->WID, &WindowIndex))
        {
            FloatingWindowLst.erase(FloatingWindowLst.begin() + WindowIndex);
            AddWindowToTree();
        }
        else
        {
            FloatingWindowLst.push_back(FocusedWindow->WID);
            RemoveWindowFromTree();
        }
    }
}

void ToggleFocusedWindowParentContainer()
{
    if(!FocusedWindow || !Screen || !DoesSpaceExistInMapOfScreen(Screen))
        return;

    space_info *Space = &Screen->Space[Screen->ActiveSpace];
    tree_node *Node = GetNodeFromWindowID(Space->RootNode, FocusedWindow->WID);
    if(Node && Node->Parent)
    {
        if(IsLeafNode(Node) && Node->Parent->WindowID == -1)
        {
            DEBUG("ToggleFocusedWindowParentContainer() Set Parent Container")
            Node->Parent->WindowID = Node->WindowID;
            ResizeWindowToContainerSize(Node->Parent);
        }
        else
        {
            DEBUG("ToggleFocusedWindowParentContainer() Restore Window Container")
            Node->Parent->WindowID = -1;
            ResizeWindowToContainerSize(Node);
        }
    }
}

void ToggleFocusedWindowFullscreen()
{
    if(!FocusedWindow || !Screen || !DoesSpaceExistInMapOfScreen(Screen))
        return;

    space_info *Space = &Screen->Space[Screen->ActiveSpace];
    if(!IsLeafNode(Space->RootNode))
    {
        tree_node *Node;
        if(Space->RootNode->WindowID == -1)
        {
            Node = GetNodeFromWindowID(Space->RootNode, FocusedWindow->WID);
            if(Node)
            {
                DEBUG("ToggleFocusedWindowFullscreen() Set fullscreen")
                Space->RootNode->WindowID = Node->WindowID;
                ResizeWindowToContainerSize(Space->RootNode);
            }
        }
        else
        {
            DEBUG("ToggleFocusedWindowFullscreen() Restore old size")
            Space->RootNode->WindowID = -1;

            Node = GetNodeFromWindowID(Space->RootNode, FocusedWindow->WID);
            if(Node)
                ResizeWindowToContainerSize(Node);
        }
    }
}

void SwapFocusedWindowWithMarked()
{
    if(!FocusedWindow || MarkedWindowID == FocusedWindow->WID || MarkedWindowID == -1)
        return;

    if(Screen && DoesSpaceExistInMapOfScreen(Screen))
    {
        space_info *Space = &Screen->Space[Screen->ActiveSpace];
        tree_node *FocusedWindowNode = GetNodeFromWindowID(Space->RootNode, FocusedWindow->WID);
        if(FocusedWindowNode)
        {
            tree_node *NewFocusNode = GetNodeFromWindowID(Space->RootNode, MarkedWindowID);;
            if(NewFocusNode)
                SwapNodeWindowIDs(FocusedWindowNode, NewFocusNode);
        }
    }

    MarkedWindowID = -1;
}

void SwapFocusedWindowWithNearest(int Shift)
{
    if(!FocusedWindow || !Screen || !DoesSpaceExistInMapOfScreen(Screen))
        return;

    space_info *Space = &Screen->Space[Screen->ActiveSpace];
    tree_node *FocusedWindowNode = GetNodeFromWindowID(Space->RootNode, FocusedWindow->WID);
    if(FocusedWindowNode)
    {
        tree_node *NewFocusNode;

        if(Shift == 1)
            NewFocusNode = GetNearestNodeToTheRight(FocusedWindowNode);
        else if(Shift == -1)
            NewFocusNode = GetNearestNodeToTheLeft(FocusedWindowNode);

        if(NewFocusNode)
            SwapNodeWindowIDs(FocusedWindowNode, NewFocusNode);
    }
}

void ShiftWindowFocus(int Shift)
{
    if(!FocusedWindow || !Screen || !DoesSpaceExistInMapOfScreen(Screen))
        return;

    space_info *Space = &Screen->Space[Screen->ActiveSpace];
    tree_node *FocusedWindowNode = GetNodeFromWindowID(Space->RootNode, FocusedWindow->WID);
    if(FocusedWindowNode)
    {
        tree_node *NewFocusNode;

        if(Shift == 1)
            NewFocusNode = GetNearestNodeToTheRight(FocusedWindowNode);
        else if(Shift == -1)
            NewFocusNode = GetNearestNodeToTheLeft(FocusedWindowNode);

        if(NewFocusNode)
        {
            window_info *NewWindow = GetWindowByID(NewFocusNode->WindowID);
            if(NewWindow)
            {
                DEBUG("ShiftWindowFocus() changing focus to " << NewWindow->Name)
                SetWindowFocus(NewWindow);
            }
        }
    }
}

void MarkWindowContainer()
{
    if(FocusedWindow)
    {
        DEBUG("MarkWindowContainer() Marked " << FocusedWindow->Name)
        MarkedWindowID = FocusedWindow->WID;
    }
}

void CloseWindowByRef(AXUIElementRef WindowRef)
{
    AXUIElementRef ActionClose;
    AXUIElementCopyAttributeValue(WindowRef, kAXCloseButtonAttribute, (CFTypeRef*)&ActionClose);
    AXUIElementPerformAction(ActionClose, kAXPressAction);
}

void CloseWindow(window_info *Window)
{
    AXUIElementRef WindowRef;
    if(GetWindowRef(Window, &WindowRef))
        CloseWindowByRef(WindowRef);
}

void SetWindowRefFocus(AXUIElementRef WindowRef, window_info *Window)
{
    ProcessSerialNumber NewPSN;
    GetProcessForPID(Window->PID, &NewPSN);

    FocusedPSN = NewPSN;
    FocusedWindowCache = *Window;
    FocusedWindow = &FocusedWindowCache;

    AXUIElementSetAttributeValue(WindowRef, kAXMainAttribute, kCFBooleanTrue);
    AXUIElementSetAttributeValue(WindowRef, kAXFocusedAttribute, kCFBooleanTrue);
    AXUIElementPerformAction(WindowRef, kAXRaiseAction);

    if(KwmFocusMode == FocusModeAutoraise)
        SetFrontProcessWithOptions(&FocusedPSN, kSetFrontProcessFrontWindowOnly);

    DEBUG("SetWindowRefFocus() Focused Window: " << FocusedWindow->Name)
}

void SetWindowFocus(window_info *Window)
{
    AXUIElementRef WindowRef;
    if(GetWindowRef(Window, &WindowRef))
        SetWindowRefFocus(WindowRef, Window);
}

void SetWindowDimensions(AXUIElementRef WindowRef, window_info *Window, int X, int Y, int Width, int Height)
{
    CGPoint WindowPos = CGPointMake(X, Y);
    CFTypeRef NewWindowPos = (CFTypeRef)AXValueCreate(kAXValueCGPointType, (const void*)&WindowPos);

    CGSize WindowSize = CGSizeMake(Width, Height);
    CFTypeRef NewWindowSize = (CFTypeRef)AXValueCreate(kAXValueCGSizeType, (void*)&WindowSize);

    AXUIElementSetAttributeValue(WindowRef, kAXPositionAttribute, NewWindowPos);
    AXUIElementSetAttributeValue(WindowRef, kAXSizeAttribute, NewWindowSize);

    Window->X = X;
    Window->Y = Y;
    Window->Width = Width;
    Window->Height = Height;

    DEBUG("SetWindowDimensions() Window " << Window->Name << ": " << Window->X << "," << Window->Y)

    if(NewWindowPos != NULL)
        CFRelease(NewWindowPos);
    if(NewWindowSize != NULL)
        CFRelease(NewWindowSize);
}

void CenterWindow(screen_info *Screen)
{
    AXUIElementRef WindowRef;
    if(GetWindowRef(FocusedWindow, &WindowRef))
    {
        int NewX = Screen->X + Screen->Width / 4;
        int NewY = Screen->Y + Screen->Height / 4;
        int NewWidth = Screen->Width / 2;
        int NewHeight = Screen->Height / 2;
        SetWindowDimensions(WindowRef, FocusedWindow, NewX, NewY, NewWidth, NewHeight);
    }
}

void MoveContainerSplitter(int SplitMode, int Offset)
{
    if(Screen && DoesSpaceExistInMapOfScreen(Screen))
    {
        space_info *Space = &Screen->Space[Screen->ActiveSpace];
        tree_node *Root = Space->RootNode;
        if(IsLeafNode(Root) || Root->WindowID != -1)
            return;

        tree_node *LeftChild = Root->LeftChild;
        tree_node *RightChild = Root->RightChild;

        if(LeftChild->Container.Type == 1 && SplitMode == 1)
        {
            DEBUG("MoveContainerSplitter() Vertical")

            LeftChild->Container.Width += Offset;
            RightChild->Container.X += Offset;
            RightChild->Container.Width -= Offset;
        }
        else if(LeftChild->Container.Type == 3 && SplitMode == 2)
        {
            DEBUG("MoveContainerSplitter() Horizontal")

            LeftChild->Container.Height += Offset;
            RightChild->Container.Y += Offset;
            RightChild->Container.Height -= Offset;
        }
        else
        {
            DEBUG("MoveContainerSplitter() Invalid")
            return;
        }

        ResizeNodeContainer(Screen, LeftChild);
        ResizeNodeContainer(Screen, RightChild);

        ApplyNodeContainer(Root);
    }
}

void ResizeWindowToContainerSize(tree_node *Node)
{
    window_info *Window = GetWindowByID(Node->WindowID);

    if(Window)
    {
        AXUIElementRef WindowRef;
        if(GetWindowRef(Window, &WindowRef))
        {
            SetWindowDimensions(WindowRef, Window,
                        Node->Container.X, Node->Container.Y, 
                        Node->Container.Width, Node->Container.Height);
        }
        else
        {
            DEBUG("GetWindowRef() Failed for window " << Window->Name)
        }
    }
}

void ResizeWindowToContainerSize()
{
    if(FocusedWindow)
    {
        if(Screen && DoesSpaceExistInMapOfScreen(Screen))
        {
            space_info *Space = &Screen->Space[Screen->ActiveSpace];
            tree_node *Node = GetNodeFromWindowID(Space->RootNode, FocusedWindow->WID);
            if(Node)
                ResizeWindowToContainerSize(Node);
        }
    }
}

CGPoint GetCursorPos()
{
    CGEventRef Event = CGEventCreate(NULL);
    CGPoint Cursor = CGEventGetLocation(Event);
    CFRelease(Event);

    return Cursor;
}

std::string GetWindowTitle(AXUIElementRef WindowRef)
{
    CFStringRef Temp;
    std::string WindowTitle;

    AXUIElementCopyAttributeValue(WindowRef, kAXTitleAttribute, (CFTypeRef*)&Temp);
    if(CFStringGetCStringPtr(Temp, kCFStringEncodingMacRoman))
        WindowTitle = CFStringGetCStringPtr(Temp, kCFStringEncodingMacRoman);

    if(Temp != NULL)
        CFRelease(Temp);

    return WindowTitle;
}

CGSize GetWindowSize(AXUIElementRef WindowRef)
{
    AXValueRef Temp;
    CGSize WindowSize;

    AXUIElementCopyAttributeValue(WindowRef, kAXSizeAttribute, (CFTypeRef*)&Temp);
    AXValueGetValue(Temp, kAXValueCGSizeType, &WindowSize);

    if(Temp != NULL)
        CFRelease(Temp);

    return WindowSize;
}

CGPoint GetWindowPos(AXUIElementRef WindowRef)
{
    AXValueRef Temp;
    CGPoint WindowPos;

    AXUIElementCopyAttributeValue(WindowRef, kAXPositionAttribute, (CFTypeRef*)&Temp);
    AXValueGetValue(Temp, kAXValueCGPointType, &WindowPos);

    if(Temp != NULL)
        CFRelease(Temp);

    return WindowPos;
}

window_info *GetWindowByID(int WindowID)
{
    for(int WindowIndex = 0; WindowIndex < WindowLst.size(); ++WindowIndex)
    {
        if(WindowLst[WindowIndex].WID == WindowID)
            return &WindowLst[WindowIndex];
    }

    return NULL;
}

bool GetWindowRole(window_info *Window, CFTypeRef *Role, CFTypeRef *SubRole)
{
    bool Result = false;

    std::map<int, window_role>::iterator It = WindowRoleCache.find(Window->WID);
    if(It != WindowRoleCache.end())
    {
        *Role = WindowRoleCache[Window->WID].Role;
        *SubRole = WindowRoleCache[Window->WID].SubRole;
        Result = true;
    }
    else
    {
        AXUIElementRef WindowRef;
        if(GetWindowRef(Window, &WindowRef))
        {
            AXUIElementCopyAttributeValue(WindowRef, kAXRoleAttribute, (CFTypeRef *)Role);
            AXUIElementCopyAttributeValue(WindowRef, kAXSubroleAttribute, (CFTypeRef *)SubRole);
            window_role RoleEntry = { *Role, *SubRole };
            WindowRoleCache[Window->WID] = RoleEntry;
            Result = true;
        }
    }

    return Result;
}

bool GetWindowRef(window_info *Window, AXUIElementRef *WindowRef)
{
    if(GetWindowRefFromCache(Window, WindowRef))
        return true;

    AXUIElementRef App = AXUIElementCreateApplication(Window->PID);
    if(!App)
    {
        DEBUG("GetWindowRef() Failed to get App for: " << Window->Name)
        return false;
    }

    CFArrayRef AppWindowLst;
    AXUIElementCopyAttributeValue(App, kAXWindowsAttribute, (CFTypeRef*)&AppWindowLst);
    if(!AppWindowLst)
    {
        DEBUG("GetWindowRef() Could not get AppWindowLst")
        return false;
    }

    bool Found = false;
    FreeWindowRefCache(Window->PID);
    CFIndex AppWindowCount = CFArrayGetCount(AppWindowLst);
    for(CFIndex WindowIndex = 0; WindowIndex < AppWindowCount; ++WindowIndex)
    {
        AXUIElementRef AppWindowRef = (AXUIElementRef)CFArrayGetValueAtIndex(AppWindowLst, WindowIndex);
        if(AppWindowRef != NULL)
        {
            WindowRefsCache[Window->PID].push_back(AppWindowRef);
            if(!Found)
            {
                int AppWindowRefWID = -1;
                _AXUIElementGetWindow(AppWindowRef, &AppWindowRefWID);
                if(AppWindowRefWID == Window->WID)
                {
                    *WindowRef = AppWindowRef;
                    Found = true;
                }
            }
        }
    }

    CFRelease(App);
    return Found;
}

bool IsApplicationInCache(int PID, std::vector<AXUIElementRef> *Elements)
{
    std::map<int, std::vector<AXUIElementRef> >::iterator It = WindowRefsCache.find(PID);
    return It != WindowRefsCache.end();
}

bool GetWindowRefFromCache(window_info *Window, AXUIElementRef *WindowRef)
{
    bool Result = false;
    std::vector<AXUIElementRef> Elements;
    bool IsCached = IsApplicationInCache(Window->PID, &Elements);

    if(IsCached)
    {
        for(int ElementIndex = 0; ElementIndex < Elements.size(); ++ElementIndex)
        {
            int AppWindowRefWID = -1;
            _AXUIElementGetWindow(Elements[ElementIndex], &AppWindowRefWID);
            if(AppWindowRefWID == Window->WID)
            {
                *WindowRef = Elements[ElementIndex];
                Result = true;
                break;
            }
        }
    }

    if(!IsCached)
        WindowRefsCache[Window->PID] = std::vector<AXUIElementRef>();

    return Result;
}

void FreeWindowRefCache(int PID)
{
    int NumElements = WindowRefsCache[PID].size();
    for(int RefIndex = 0; RefIndex < NumElements; ++RefIndex)
        CFRelease(WindowRefsCache[PID][RefIndex]);

    WindowRefsCache[PID].clear();
}

void GetWindowInfo(const void *Key, const void *Value, void *Context)
{
    CFStringRef K = (CFStringRef)Key;
    std::string KeyStr = CFStringGetCStringPtr(K, kCFStringEncodingMacRoman);

    CFTypeID ID = CFGetTypeID(Value);
    if(ID == CFStringGetTypeID())
    {
        CFStringRef V = (CFStringRef)Value;
        if(CFStringGetCStringPtr(V, kCFStringEncodingMacRoman))
        {
            std::string ValueStr = CFStringGetCStringPtr(V, kCFStringEncodingMacRoman);
            if(KeyStr == "kCGWindowName")
                WindowLst[WindowLst.size()-1].Name = ValueStr;
            else if(KeyStr == "kCGWindowOwnerName")
                WindowLst[WindowLst.size()-1].Owner = ValueStr;
        }
    }
    else if(ID == CFNumberGetTypeID())
    {
        int MyInt;
        CFNumberRef V = (CFNumberRef)Value;
        CFNumberGetValue(V, kCFNumberSInt64Type, &MyInt);
        if(KeyStr == "kCGWindowNumber")
            WindowLst[WindowLst.size()-1].WID = MyInt;
        else if(KeyStr == "kCGWindowOwnerPID")
            WindowLst[WindowLst.size()-1].PID = MyInt;
        else if(KeyStr == "kCGWindowLayer")
            WindowLst[WindowLst.size()-1].Layer = MyInt;
        else if(KeyStr == "X")
            WindowLst[WindowLst.size()-1].X = MyInt;
        else if(KeyStr == "Y")
            WindowLst[WindowLst.size()-1].Y = MyInt;
        else if(KeyStr == "Width")
            WindowLst[WindowLst.size()-1].Width = MyInt;
        else if(KeyStr == "Height")
            WindowLst[WindowLst.size()-1].Height = MyInt;
    }
    else if(ID == CFDictionaryGetTypeID())
    {
        CFDictionaryRef Elem = (CFDictionaryRef)Value;
        CFDictionaryApplyFunction(Elem, GetWindowInfo, NULL);
    } 
}
