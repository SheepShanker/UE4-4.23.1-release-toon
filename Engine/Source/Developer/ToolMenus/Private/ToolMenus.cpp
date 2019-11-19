// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "ToolMenus.h"
#include "IToolMenusModule.h"

#include "Textures/SlateIcon.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/MultiBox/MultiBox.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Application/SlateApplication.h"
#include "Internationalization/Internationalization.h"

#include "HAL/PlatformApplicationMisc.h" // For clipboard
#include "Widgets/Layout/SScrollBox.h"

#define LOCTEXT_NAMESPACE "ToolMenuSubsystem"

DEFINE_LOG_CATEGORY(LogToolMenus);

UToolMenus* UToolMenus::Singleton = nullptr;
bool UToolMenus::bHasShutDown = false;
FSimpleMulticastDelegate UToolMenus::StartupCallbacks;
TOptional<FDelegateHandle> UToolMenus::InternalStartupCallbackHandle;

FAutoConsoleCommand ToolMenusRefreshMenuWidget = FAutoConsoleCommand(
	TEXT("ToolMenus.RefreshAllWidgets"),
	TEXT("Refresh All Tool Menu Widgets"),
	FConsoleCommandDelegate::CreateLambda([]() {
		UToolMenus::Get()->RefreshAllWidgets();
	}));

FName FToolMenuStringCommand::GetTypeName() const
{
	static const FName CommandName("Command");
	static const FName PythonName("Python");

	switch (Type)
	{
	case EToolMenuStringCommandType::Command:
		return CommandName;
	case EToolMenuStringCommandType::Python:
		return PythonName;
	case EToolMenuStringCommandType::Custom:
		return CustomType;
	default:
		break;
	}

	return NAME_None;
}

FExecuteAction FToolMenuStringCommand::ToExecuteAction(const FToolMenuContext& Context) const
{
	if (IsBound())
	{
		return FExecuteAction::CreateStatic(&UToolMenus::ExecuteStringCommand, *this, Context);
	}

	return FExecuteAction();
}

FToolUIActionChoice::FToolUIActionChoice(const TSharedPtr< const FUICommandInfo >& InCommand, const FUICommandList& InCommandList)
{
	if (InCommand.IsValid())
	{
		if (const FUIAction* UIAction = InCommandList.GetActionForCommand(InCommand))
		{
			Action = *UIAction;
			ToolAction.Reset();
			DynamicToolAction.Reset();
		}
	}
}

UToolMenus::UToolMenus() :
	bNextTickTimerIsSet(false),
	bRefreshWidgetsNextTick(false),
	bCleanupStaleWidgetsNextTick(false),
	bEditMenusMode(false)
{
}

UToolMenus* UToolMenus::Get()
{
	if (!Singleton && !bHasShutDown)
	{
		// Required for StartupModule and ShutdownModule to be called and FindModule to list the ToolsMenus module
		FModuleManager::LoadModuleChecked<IToolMenusModule>("ToolMenus");

		Singleton = NewObject<UToolMenus>();
		Singleton->AddToRoot();
		check(Singleton);
	}

	return Singleton;
}

void UToolMenus::BeginDestroy()
{
	if (Singleton == this)
	{
		UnregisterPrivateStartupCallback();

		bHasShutDown = true;
		Singleton = nullptr;
	}

	Super::BeginDestroy();
}

bool UToolMenus::IsToolMenuUIEnabled()
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	return !IsRunningCommandlet() && !IsRunningGame() && !IsRunningDedicatedServer() && !IsRunningClientOnly();
}

FName UToolMenus::JoinMenuPaths(const FName Base, const FName Child)
{
	return *(Base.ToString() + TEXT(".") + Child.ToString());
}

bool UToolMenus::SplitMenuPath(const FName MenuPath, FName& OutLeft, FName& OutRight)
{
	if (MenuPath != NAME_None)
	{
		FString Left;
		FString Right;
		if (MenuPath.ToString().Split(TEXT("."), &Left, &Right, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			OutLeft = *Left;
			OutRight = *Right;
			return true;
		}
	}

	return false;
}

bool UToolMenus::GetDisplayUIExtensionPoints() const
{
	return ShouldDisplayExtensionPoints.IsBound() && ShouldDisplayExtensionPoints.Execute();
}

UToolMenu* UToolMenus::FindMenu(const FName Name)
{
	UToolMenu** Found = Menus.Find(Name);
	return Found ? *Found : nullptr;
}

bool UToolMenus::IsMenuRegistered(const FName Name) const
{
	const UToolMenu* const * Found = Menus.Find(Name);
	return Found && *Found && (*Found)->IsRegistered();
}

TArray<UToolMenu*> UToolMenus::CollectHierarchy(const FName InName)
{
	TArray<UToolMenu*> Result;

	UToolMenu* Current = FindMenu(InName);
	while (Current)
	{
		// Detect infinite loop
		for (UToolMenu* Other : Result)
		{
			if (Other->MenuName == Current->MenuName)
			{
				UE_LOG(LogToolMenus, Warning, TEXT("Infinite loop detected in tool menu: %s"), *InName.ToString());
				return TArray<UToolMenu*>();
			}
		}

		Result.Add(Current);

		if (Current->MenuParent != NAME_None)
		{
			Current = FindMenu(Current->MenuParent);
		}
		else
		{
			break;
		}
	}

	Algo::Reverse(Result);

	return Result;
}

void UToolMenus::ListAllParents(const FName InName, TArray<FName>& AllParents)
{
	for (const UToolMenu* Menu : CollectHierarchy(InName))
	{
		AllParents.Add(Menu->MenuName);
	}
}

void UToolMenus::AssembleMenuSection(UToolMenu* GeneratedMenu, const UToolMenu* Other, FToolMenuSection* DestSection, const FToolMenuSection& OtherSection)
{
	// Build list of blocks in expected order including blocks created by construct delegates
	TArray<FToolMenuEntry> RemainingBlocks;
	TArray<FToolMenuEntry> BlocksToAddLast;

	UToolMenu* ConstructedEntries = nullptr;
	for (const FToolMenuEntry& Block : OtherSection.Blocks)
	{
		if (!Block.IsNonLegacyDynamicConstruct())
		{
			if (Block.bAddedDuringRegister)
			{
				RemainingBlocks.Add(Block);
			}
			else
			{
				BlocksToAddLast.Add(Block);
			}
			continue;
		}

		if (ConstructedEntries == nullptr)
		{
			ConstructedEntries = NewObject<UToolMenu>(this);
			ConstructedEntries->Context = DestSection->Context;
		}

		TArray<FToolMenuEntry> GeneratedEntries;
		GeneratedEntries.Add(Block);

		int32 NumIterations = 0;
		while (GeneratedEntries.Num() > 0)
		{
			FToolMenuEntry& GeneratedEntry = GeneratedEntries[0];
			if (GeneratedEntry.IsNonLegacyDynamicConstruct())
			{
				if (NumIterations++ > 5000)
				{
					UE_LOG(LogToolMenus, Warning, TEXT("Possible infinite loop for menu: %s, section: %s, block: %s"), *Other->MenuName.ToString(), *OtherSection.Name.ToString(), *Block.Name.ToString());
					break;
				}
				
				ConstructedEntries->Sections.Reset();
				if (GeneratedEntry.IsScriptObjectDynamicConstruct())
				{
					GeneratedEntry.ScriptObject->ConstructMenuEntry(ConstructedEntries, DestSection->Name, DestSection->Context);
				}
				else
				{
					FToolMenuSection& ConstructedSection = ConstructedEntries->AddSection(DestSection->Name);
					ConstructedSection.Context = ConstructedEntries->Context;
					GeneratedEntry.Construct.Execute(ConstructedSection);
				}
				GeneratedEntries.RemoveAt(0, 1, false);

				// Combine all user's choice of selections here into the current section target
				// If the user wants to add items to different sections they will need to create dynamic section instead (for now)
				int32 NumBlocksInserted = 0;
				for (FToolMenuSection& ConstructedSection : ConstructedEntries->Sections)
				{
					for (FToolMenuEntry& ConstructedBlock : ConstructedSection.Blocks)
					{
						if (ConstructedBlock.InsertPosition.IsDefault())
						{
							ConstructedBlock.InsertPosition = Block.InsertPosition;
						}
					}
					GeneratedEntries.Insert(ConstructedSection.Blocks, NumBlocksInserted);
					NumBlocksInserted += ConstructedSection.Blocks.Num();
				}
			}
			else
			{
				if (Block.bAddedDuringRegister)
				{
					RemainingBlocks.Add(GeneratedEntry);
				}
				else
				{
					BlocksToAddLast.Add(GeneratedEntry);
				}
				GeneratedEntries.RemoveAt(0, 1, false);
			}
		}
	}

	if (ConstructedEntries)
	{
		ConstructedEntries->Sections.Empty();
		ConstructedEntries = nullptr;
	}

	RemainingBlocks.Append(BlocksToAddLast);

	// Repeatedly loop because insert location may not exist until later in list
	while (RemainingBlocks.Num() > 0)
	{
		int32 NumHandled = 0;
		for (int32 i = 0; i < RemainingBlocks.Num(); ++i)
		{
			FToolMenuEntry& Block = RemainingBlocks[i];
			int32 DestIndex = DestSection->FindBlockInsertIndex(Block);
			if (DestIndex != INDEX_NONE)
			{
				DestSection->Blocks.Insert(Block, DestIndex);
				RemainingBlocks.RemoveAt(i);
				--i;
				++NumHandled;
				// Restart loop because items earlier in the list may need to attach to this block
				break;
			}
		}
		if (NumHandled == 0)
		{
			for (const FToolMenuEntry& Block : RemainingBlocks)
			{
				UE_LOG(LogToolMenus, Warning, TEXT("Menu item not found: '%s' for insert: '%s'"), *Block.InsertPosition.Name.ToString(), *Block.Name.ToString());
			}
			break;
		}
	}
}

void UToolMenus::AssembleMenu(UToolMenu* GeneratedMenu, const UToolMenu* Other)
{
	TArray<FToolMenuSection> RemainingSections;

	UToolMenu* ConstructedSections = nullptr;
	for (const FToolMenuSection& OtherSection : Other->Sections)
	{
		if (!OtherSection.IsNonLegacyDynamic())
		{
			RemainingSections.Add(OtherSection);
			continue;
		}
		
		if (ConstructedSections == nullptr)
		{
			ConstructedSections = NewObject<UToolMenu>(this);
			ConstructedSections->Context = GeneratedMenu->Context;
		}

		TArray<FToolMenuSection> GeneratedSections;
		GeneratedSections.Add(OtherSection);

		int32 NumIterations = 0;
		while (GeneratedSections.Num() > 0)
		{
			if (GeneratedSections[0].IsNonLegacyDynamic())
			{
				if (NumIterations++ > 5000)
				{
					UE_LOG(LogToolMenus, Warning, TEXT("Possible infinite loop for menu: %s, section: %s"), *Other->MenuName.ToString(), *OtherSection.Name.ToString());
					break;
				}

				ConstructedSections->Sections.Reset();
				
				if (GeneratedSections[0].ToolMenuSectionDynamic)
				{
					GeneratedSections[0].ToolMenuSectionDynamic->ConstructSections(ConstructedSections, GeneratedMenu->Context);
				}
				else if (GeneratedSections[0].Construct.NewToolMenuDelegate.IsBound())
				{
					GeneratedSections[0].Construct.NewToolMenuDelegate.Execute(ConstructedSections);
				}

				for (FToolMenuSection& ConstructedSection : ConstructedSections->Sections)
				{
					if (ConstructedSection.InsertPosition.IsDefault())
					{
						ConstructedSection.InsertPosition = GeneratedSections[0].InsertPosition;
					}
				}
				
				GeneratedSections.RemoveAt(0, 1, false);				
				GeneratedSections.Insert(ConstructedSections->Sections, 0);
			}
			else
			{
				RemainingSections.Add(GeneratedSections[0]);
				GeneratedSections.RemoveAt(0, 1, false);
			}
		}
	}

	if (ConstructedSections)
	{
		ConstructedSections->Sections.Empty();
		ConstructedSections = nullptr;
	}

	while (RemainingSections.Num() > 0)
	{
		int32 NumHandled = 0;
		for (int32 i=0; i < RemainingSections.Num(); ++i)
		{
			const FToolMenuSection& RemainingSection = RemainingSections[i];

			// Update existing section
			FToolMenuSection* Section = GeneratedMenu->FindSection(RemainingSection.Name);
			if (!Section)
			{
				// Try add new section (if insert location exists)
				int32 DestIndex = GeneratedMenu->FindInsertIndex(RemainingSection);
				if (DestIndex != INDEX_NONE)
				{
					GeneratedMenu->Sections.InsertDefaulted(DestIndex);
					Section = &GeneratedMenu->Sections[DestIndex];
					Section->InitGeneratedSectionCopy(RemainingSection, GeneratedMenu->Context);
				}
				else
				{
					continue;
				}
			}
			else
			{
				// Allow overriding label
				if (!Section->Label.IsSet() && RemainingSection.Label.IsSet())
				{
					Section->Label = RemainingSection.Label;
				}
			}

			AssembleMenuSection(GeneratedMenu, Other, Section, RemainingSection);
			RemainingSections.RemoveAt(i);
			--i;
			++NumHandled;
			break;
		}
		if (NumHandled == 0)
		{
			for (const FToolMenuSection& RemainingSection : RemainingSections)
			{
				UE_LOG(LogToolMenus, Warning, TEXT("Menu section not found: '%s' for insert: '%s'"), *RemainingSection.InsertPosition.Name.ToString(), *RemainingSection.Name.ToString());
			}
			break;
		}
	}
}

bool UToolMenus::GetEditMenusMode() const
{
	return bEditMenusMode;
}

void UToolMenus::SetEditMenusMode(bool bShow)
{
	if (bEditMenusMode != bShow)
	{
		bEditMenusMode = bShow;
		RefreshAllWidgets();
	}
}

void UToolMenus::RemoveCustomization(const FName InName)
{
	int32 FoundIndex = FindMenuCustomizationIndex(InName);
	if (FoundIndex != INDEX_NONE)
	{
		CustomizedMenus.RemoveAt(FoundIndex, 1, false);
	}
}

int32 UToolMenus::FindMenuCustomizationIndex(const FName InName)
{
	for (int32 i = 0; i < CustomizedMenus.Num(); ++i)
	{
		if (CustomizedMenus[i].Name == InName)
		{
			return i;
		}
	}

	return INDEX_NONE;
}

FCustomizedToolMenu* UToolMenus::FindMenuCustomization(const FName InName)
{
	for (int32 i = 0; i < CustomizedMenus.Num(); ++i)
	{
		if (CustomizedMenus[i].Name == InName)
		{
			return &CustomizedMenus[i];
		}
	}

	return nullptr;
}

FCustomizedToolMenu* UToolMenus::AddMenuCustomization(const FName InName)
{
	if (FCustomizedToolMenu* Found = FindMenuCustomization(InName))
	{
		return Found;
	}
	else
	{
		FCustomizedToolMenu& NewCustomization = CustomizedMenus.AddDefaulted_GetRef();
		NewCustomization.Name = InName;
		return &NewCustomization;
	}
}

void UToolMenus::ApplyCustomization(UToolMenu* GeneratedMenu)
{
	FCustomizedToolMenuHierarchy CustomizationHierarchy = GeneratedMenu->GetMenuCustomizationHierarchy();
	if (CustomizationHierarchy.Hierarchy.Num() == 0)
	{
		return;
	}
	FCustomizedToolMenu CustomizedMenu = CustomizationHierarchy.GenerateFlattened();

	TArray<FToolMenuSection> NewSections;
	NewSections.Reserve(GeneratedMenu->Sections.Num());

	TSet<FName> PlacedEntries;

	TArray<int32> NewSectionIndices;
	NewSectionIndices.Reserve(GeneratedMenu->Sections.Num());

	// Add sections with customized ordering first
	for (const FName SectionName : CustomizedMenu.SectionOrder)
	{
		if (SectionName != NAME_None)
		{
			int32 OriginalIndex = GeneratedMenu->Sections.IndexOfByPredicate([SectionName](const FToolMenuSection& OriginalSection) { return OriginalSection.Name == SectionName; });
			if (OriginalIndex != INDEX_NONE)
			{
				NewSectionIndices.Add(OriginalIndex);
			}
		}
	}

	// Remaining sections get added to the end
	for (int32 i = 0; i < GeneratedMenu->Sections.Num(); ++i)
	{
		NewSectionIndices.AddUnique(i);
	}

	// Copy sections
	for (int32 i = 0; i < NewSectionIndices.Num(); ++i)
	{
		FToolMenuSection& NewSection = NewSections.Add_GetRef(GeneratedMenu->Sections[NewSectionIndices[i]]);
		NewSection.Blocks.Reset();
	}

	// Add entries placed by customization
	for (int32 i = 0; i < NewSectionIndices.Num(); ++i)
	{
		const FToolMenuSection& OriginalSection = GeneratedMenu->Sections[NewSectionIndices[i]];
		FToolMenuSection& NewSection = NewSections[i];

		if (OriginalSection.Name != NAME_None)
		{
			if (FCustomizedToolMenuNameArray* EntryOrder = CustomizedMenu.EntryOrder.Find(OriginalSection.Name))
			{
				for (const FName EntryName : EntryOrder->Names)
				{
					if (EntryName != NAME_None)
					{
						if (FToolMenuEntry* SourceEntry = GeneratedMenu->FindEntry(EntryName))
						{
							NewSection.Blocks.Add(*SourceEntry);
							PlacedEntries.Add(EntryName);
						}
					}
				}
			}
		}
	}

	// Handle entries not placed by customization
	for (int32 i = 0; i < NewSectionIndices.Num(); ++i)
	{
		const FToolMenuSection& OriginalSection = GeneratedMenu->Sections[NewSectionIndices[i]];
		FToolMenuSection& NewSection = NewSections[i];

		for (const FToolMenuEntry& OriginalEntry : OriginalSection.Blocks)
		{
			if (OriginalEntry.Name == NAME_None)
			{
				NewSection.Blocks.Add(OriginalEntry);
			}
			else
			{
				bool bAlreadyPlaced = false;
				PlacedEntries.Add(OriginalEntry.Name, &bAlreadyPlaced);
				if (!bAlreadyPlaced)
				{
					NewSection.Blocks.Add(OriginalEntry);
				}
			}
		}
	}

	// Hide items based on whitelist
	if (CustomizedMenu.WhitelistEntries.Num() > 0)
	{
		for (int32 SectionIndex = 0; SectionIndex < NewSections.Num(); ++SectionIndex)
		{
			FToolMenuSection& Section = NewSections[SectionIndex];
			for (int32 i = 0; i < Section.Blocks.Num(); ++i)
			{
				if (!CustomizedMenu.IsEntryWhitelisted(Section.Blocks[i].Name))
				{
					Section.Blocks.RemoveAt(i);
					--i;
				}
			}
		}
	}

	// Hide sections and entries
	if (!GeneratedMenu->IsEditing())
	{
		for (int32 SectionIndex = 0; SectionIndex < NewSections.Num(); ++SectionIndex)
		{
			FToolMenuSection& Section = NewSections[SectionIndex];
			if (CustomizedMenu.IsSectionHidden(Section.Name))
			{
				NewSections.RemoveAt(SectionIndex);
				--SectionIndex;
				continue;
			}

			for (int32 i = 0; i < Section.Blocks.Num(); ++i)
			{
				if (CustomizedMenu.IsEntryHidden(Section.Blocks[i].Name))
				{
					Section.Blocks.RemoveAt(i);
					--i;
				}
			}
		}
	}

	GeneratedMenu->Sections = NewSections;
}

void UToolMenus::AssembleMenuHierarchy(UToolMenu* GeneratedMenu, const TArray<UToolMenu*>& Hierarchy)
{
	if (GeneratedMenu->MenuType == EMultiBoxType::MenuBar)
	{
		// Menu Bars require one section
		if (GeneratedMenu->Sections.Num() == 0)
		{
			GeneratedMenu->Sections.AddDefaulted();
		}

		FToolMenuSection& MenuBarSection = GeneratedMenu->Sections[0];
		for (const UToolMenu* MenuData : Hierarchy)
		{
			for (const FToolMenuSection& Section : MenuData->Sections)
			{
				for (const FToolMenuEntry& Block : Section.Blocks)
				{
					MenuBarSection.AssembleBlock(Block);
				}
			}
		}
	}
	else
	{
		for (const UToolMenu* FoundParent : Hierarchy)
		{
			AssembleMenu(GeneratedMenu, FoundParent);
		}
	}

	ApplyCustomization(GeneratedMenu);
}

UToolMenu* UToolMenus::GenerateSubMenu(const UToolMenu* InGeneratedParent, const FName InBlockName)
{
	if (InGeneratedParent == nullptr || InBlockName == NAME_None)
	{
		return nullptr;
	}

	FName SubMenuFullName = JoinMenuPaths(InGeneratedParent->GetMenuName(), InBlockName);
	if (const FName* ReplacementName = MenuSubstitutionsDuringGenerate.Find(SubMenuFullName))
	{
		return GenerateMenuFromHierarchy(CollectHierarchy(*ReplacementName), InGeneratedParent->Context);
	}

	const FToolMenuEntry* Block = InGeneratedParent->FindEntry(InBlockName);
	if (!Block)
	{
		return nullptr;
	}


	// Submenus that are constructed by delegates can also be overridden by menus in the database
	TArray<UToolMenu*> Hierarchy;
	{
		// Walk up all parent menus trying to find a menu
		FName BaseName = InGeneratedParent->GetMenuName();
		while (BaseName != NAME_None)
		{
			FName JoinedName = JoinMenuPaths(BaseName, InBlockName);
			if (UToolMenu* Found = FindMenu(JoinedName))
			{
				Hierarchy = CollectHierarchy(JoinedName);
				break;
			}

			UToolMenu* BaseData = FindMenu(BaseName);
			BaseName = BaseData ? BaseData->MenuParent : NAME_None;
		}
	}

	// Construct menu using delegate and insert as root so it can be overridden
	if (Block->SubMenuData.ConstructMenu.NewToolMenuDelegate.IsBound())
	{
		UToolMenu* Menu = NewObject<UToolMenu>(this);
		Menu->Context = InGeneratedParent->Context;
		Menu->MenuName = SubMenuFullName;

		// Submenu specific data
		Menu->SubMenuParent = InGeneratedParent;
		Menu->SubMenuSourceEntryName = InBlockName;

		Block->SubMenuData.ConstructMenu.NewToolMenuDelegate.Execute(Menu);
		Menu->MenuName = SubMenuFullName;
		Hierarchy.Insert(Menu, 0);
	}

	// Populate menu builder with final menu
	if (Hierarchy.Num() > 0)
	{
		UToolMenu* GeneratedMenu = NewObject<UToolMenu>(this);
		GeneratedMenu->InitGeneratedCopy(Hierarchy[0], SubMenuFullName, &InGeneratedParent->Context);
		for (UToolMenu* HiearchyItem : Hierarchy)
		{
			if (HiearchyItem && !HiearchyItem->bExtendersEnabled)
			{
				GeneratedMenu->SetExtendersEnabled(false);
				break;
			}
		}
		GeneratedMenu->SubMenuParent = InGeneratedParent;
		GeneratedMenu->SubMenuSourceEntryName = InBlockName;
		AssembleMenuHierarchy(GeneratedMenu, Hierarchy);
		return GeneratedMenu;
	}

	return nullptr;
}

void UToolMenus::PopulateSubMenu(FMenuBuilder& MenuBuilder, TWeakObjectPtr<UToolMenu> InParent, const FName InBlockName)
{
	if (UToolMenu* GeneratedMenu = GenerateSubMenu(InParent.Get(), InBlockName))
	{
		MenuBuilder.SetExtendersEnabled(GeneratedMenu->bExtendersEnabled);
		PopulateMenuBuilder(MenuBuilder, GeneratedMenu);
	}
}

void UToolMenus::PopulateSubMenuWithoutName(FMenuBuilder& MenuBuilder, TWeakObjectPtr<UToolMenu> InParent, const FNewToolMenuDelegate InNewToolMenuDelegate)
{
	const UToolMenu* InGeneratedParent = InParent.Get();
	if (InGeneratedParent == nullptr)
	{
		return;
	}

	if (InNewToolMenuDelegate.IsBound())
	{
		UToolMenu* Menu = NewObject<UToolMenu>(this);
		Menu->Context = InGeneratedParent->Context;
		Menu->MenuName = NAME_None; // Menu does not have a name

		// Submenu specific data
		Menu->SubMenuParent = InGeneratedParent;
		Menu->SubMenuSourceEntryName = NAME_None; // Entry does not have a name

		InNewToolMenuDelegate.Execute(Menu);
		Menu->MenuName = NAME_None; // Menu does not have a name

		PopulateMenuBuilder(MenuBuilder, Menu);
	}
}

TSharedRef<SWidget> UToolMenus::GenerateToolbarComboButtonMenu(TWeakObjectPtr<UToolMenu> InParent, const FName InBlockName)
{
	if (UToolMenu* GeneratedMenu = GenerateSubMenu(InParent.Get(), InBlockName))
	{
		return GenerateWidget(GeneratedMenu);
	}

	return SNullWidget::NullWidget;
}

void UToolMenus::PopulateMenuBuilder(FMenuBuilder& MenuBuilder, UToolMenu* MenuData)
{
	const bool bIsEditing = MenuData->IsEditing();
	if (GetEditMenusMode() && !bIsEditing && EditMenuDelegate.IsBound())
	{
		TWeakObjectPtr<UToolMenu> WeakMenuPtr = MenuData;
		const FName MenuName = MenuData->GetMenuName();
		MenuBuilder.AddMenuEntry(
			FText::Format(LOCTEXT("EditMenu_Label", "Edit Menu: {0}"), FText::FromName(MenuName)),
			LOCTEXT("EditMenu_ToolTip", "Open menu editor"),
			EditMenuIcon,
			FExecuteAction::CreateLambda([MenuName, WeakMenuPtr]()
			{
				FPlatformApplicationMisc::ClipboardCopy(*MenuName.ToString());
				if (UToolMenu* InMenu = WeakMenuPtr.Get())
				{
					UToolMenus::Get()->EditMenuDelegate.ExecuteIfBound(InMenu);
				}
			}),
			"MenuName"
		);
	}

	for (int i=0; i < MenuData->Sections.Num(); ++i)
	{
		FToolMenuSection& Section = MenuData->Sections[i];
		if (Section.Construct.NewToolMenuDelegateLegacy.IsBound())
		{
			if (!bIsEditing)
			{
				Section.Construct.NewToolMenuDelegateLegacy.Execute(MenuBuilder, MenuData);
			}
			continue;
		}

		if (bIsEditing)
		{
			// Always provide label when editing so we have area to drag/drop and hide sections
			FText LabelText = Section.Label.Get();
			if (LabelText.IsEmpty())
			{
				LabelText = FText::FromName(Section.Name);
			}
			MenuBuilder.BeginSection(Section.Name, LabelText);
		}
		else
		{
			MenuBuilder.BeginSection(Section.Name, Section.Label);
		}

		for (FToolMenuEntry& Block : Section.Blocks)
		{
			if (Block.ConstructLegacy.IsBound())
			{
				if (!bIsEditing)
				{
					Block.ConstructLegacy.Execute(MenuBuilder, MenuData);
				}
				continue;
			}

			FUIAction UIAction = ConvertUIAction(Block, MenuData->Context);

			TSharedPtr<SWidget> Widget;
			if (Block.MakeWidget.IsBound())
			{
				Widget = Block.MakeWidget.Execute(MenuData->Context);
			}

			TAttribute<FText> LabelToDisplay = Block.Label;
			if (bIsEditing && (!Block.Label.IsSet() || Block.Label.Get().IsEmpty()))
			{
				LabelToDisplay = FText::FromName(Block.Name);
			}

			if (Block.Type == EMultiBlockType::MenuEntry)
			{
				if (Block.IsSubMenu())
				{
					FName SubMenuFullName = JoinMenuPaths(MenuData->MenuName, Block.Name);
					FNewMenuDelegate NewMenuDelegate;
					if (Block.SubMenuData.ConstructMenu.NewMenuDelegate.IsBound())
					{
						NewMenuDelegate = Block.SubMenuData.ConstructMenu.NewMenuDelegate;
					}
					else if (Block.Name == NAME_None)
					{
						if (Block.SubMenuData.ConstructMenu.NewToolMenuDelegate.IsBound())
						{
							// Blocks with no name cannot call PopulateSubMenu()
							NewMenuDelegate = FNewMenuDelegate::CreateUObject(this, &UToolMenus::PopulateSubMenuWithoutName, TWeakObjectPtr<UToolMenu>(MenuData), Block.SubMenuData.ConstructMenu.NewToolMenuDelegate);
						}
						else
						{
							UE_LOG(LogToolMenus, Warning, TEXT("Submenu that has no name is missing required delegate: %s"), *MenuData->MenuName.ToString());
						}
					}
					else
					{
						NewMenuDelegate = FNewMenuDelegate::CreateUObject(this, &UToolMenus::PopulateSubMenu, TWeakObjectPtr<UToolMenu>(MenuData), Block.Name);
					}

					if (Widget.IsValid())
					{
						if (UIAction.IsBound())
						{
							MenuBuilder.AddSubMenu(UIAction, Widget.ToSharedRef(), NewMenuDelegate, Block.bShouldCloseWindowAfterMenuSelection);
						}
						else
						{
							MenuBuilder.AddSubMenu(Widget.ToSharedRef(), NewMenuDelegate, Block.SubMenuData.bOpenSubMenuOnClick, Block.bShouldCloseWindowAfterMenuSelection);
						}
					}
					else
					{
						if (UIAction.IsBound())
						{
							MenuBuilder.AddSubMenu(
								Block.Label,
								Block.ToolTip,
								NewMenuDelegate,
								UIAction,
								Block.Name,
								Block.UserInterfaceActionType,
								Block.SubMenuData.bOpenSubMenuOnClick,
								Block.Icon.Get(),
								Block.bShouldCloseWindowAfterMenuSelection
							);
						}
						else
						{
							MenuBuilder.AddSubMenu(
								Block.Label,
								Block.ToolTip,
								NewMenuDelegate,
								Block.SubMenuData.bOpenSubMenuOnClick,
								Block.Icon.Get(),
								Block.bShouldCloseWindowAfterMenuSelection,
								Block.Name
							);
						}
					}
				}
				else
				{
					if (Block.Command.IsValid())
					{
						bool bPopCommandList = false;
						TSharedPtr<const FUICommandList> CommandListForAction;
						if (Block.GetActionForCommand(MenuData->Context, CommandListForAction) != nullptr && CommandListForAction.IsValid())
						{
							MenuBuilder.PushCommandList(CommandListForAction.ToSharedRef());
							bPopCommandList = true;
						}
						else
						{
							UE_LOG(LogToolMenus, Error, TEXT("UI command not found for menu entry: %s, menu: %s"), *Block.Name.ToString(), *MenuData->MenuName.ToString());
						}

						MenuBuilder.AddMenuEntry(Block.Command, Block.Name, LabelToDisplay, Block.ToolTip, Block.Icon.Get());

						if (bPopCommandList)
						{
							MenuBuilder.PopCommandList();
						}
					}
					else if (Block.ScriptObject)
					{
						UToolMenuEntryScript* ScriptObject = Block.ScriptObject;
						const FSlateIcon Icon = ScriptObject->CreateIconAttribute(MenuData->Context).Get();
						MenuBuilder.AddMenuEntry(ScriptObject->CreateLabelAttribute(MenuData->Context), ScriptObject->CreateToolTipAttribute(MenuData->Context), Icon, UIAction, ScriptObject->Data.Name, Block.UserInterfaceActionType, Block.TutorialHighlightName);
					}
					else
					{
						if (Widget.IsValid())
						{
							MenuBuilder.AddMenuEntry(UIAction, Widget.ToSharedRef(), Block.Name, Block.ToolTip, Block.UserInterfaceActionType, Block.TutorialHighlightName);
						}
						else
						{
							MenuBuilder.AddMenuEntry(LabelToDisplay, Block.ToolTip, Block.Icon.Get(), UIAction, Block.Name, Block.UserInterfaceActionType, Block.TutorialHighlightName);
						}
					}
				}
			}
			else if (Block.Type == EMultiBlockType::MenuSeparator)
			{
				MenuBuilder.AddMenuSeparator(Block.Name);
			}
			else if (Block.Type == EMultiBlockType::Widget)
			{
				if (bIsEditing)
				{
					MenuBuilder.AddMenuEntry(LabelToDisplay, Block.ToolTip, Block.Icon.Get(), UIAction, Block.Name, Block.UserInterfaceActionType, Block.TutorialHighlightName);
				}
				else
				{
					MenuBuilder.AddWidget(Widget.ToSharedRef(), LabelToDisplay.Get(), Block.WidgetData.bNoIndent, Block.WidgetData.bSearchable);
				}
			}
			else
			{
				UE_LOG(LogToolMenus, Warning, TEXT("Menu '%s', item '%s', type not currently supported: %d"), *MenuData->MenuName.ToString(), *Block.Name.ToString(), Block.Type);
			}
		}

		MenuBuilder.EndSection();
	}

	AddReferencedContextObjects(MenuBuilder.GetMultiBox(), MenuData);
}

void UToolMenus::PopulateToolBarBuilder(FToolBarBuilder& ToolBarBuilder, UToolMenu* MenuData)
{
	if (GetEditMenusMode() && !MenuData->IsEditing() && EditMenuDelegate.IsBound())
	{
		TWeakObjectPtr<UToolMenu> WeakMenuPtr = MenuData;
		const FName MenuName = MenuData->GetMenuName();
		ToolBarBuilder.BeginSection(MenuName);
		ToolBarBuilder.AddToolBarButton(
			FExecuteAction::CreateLambda([MenuName, WeakMenuPtr]()
			{
				FPlatformApplicationMisc::ClipboardCopy(*MenuName.ToString());
				if (UToolMenu* InMenu = WeakMenuPtr.Get())
				{
					UToolMenus::Get()->EditMenuDelegate.ExecuteIfBound(InMenu);
				}
			}), 
			"MenuName",
			LOCTEXT("EditMenu", "Edit Menu"),
			LOCTEXT("EditMenu_ToolTip", "Open menu editor"),
			EditToolbarIcon
		);
		ToolBarBuilder.EndSection();
	}

	for (FToolMenuSection& Section : MenuData->Sections)
	{
		if (Section.Construct.NewToolBarDelegateLegacy.IsBound())
		{
			Section.Construct.NewToolBarDelegateLegacy.Execute(ToolBarBuilder, MenuData);
			continue;
		}

		ToolBarBuilder.BeginSection(Section.Name);

		for (FToolMenuEntry& Block : Section.Blocks)
		{
			if (Block.ToolBarData.ConstructLegacy.IsBound())
			{
				Block.ToolBarData.ConstructLegacy.Execute(ToolBarBuilder, MenuData);
				continue;
			}

			FUIAction UIAction = ConvertUIAction(Block, MenuData->Context);

			TSharedPtr<SWidget> Widget;
			if (Block.MakeWidget.IsBound())
			{
				Widget = Block.MakeWidget.Execute(MenuData->Context);
			}

			if (Block.Type == EMultiBlockType::ToolBarButton)
			{
				if (Block.Command.IsValid())
				{
					bool bPopCommandList = false;
					TSharedPtr<const FUICommandList> CommandListForAction;
					if (Block.GetActionForCommand(MenuData->Context, CommandListForAction) != nullptr && CommandListForAction.IsValid())
					{
						ToolBarBuilder.PushCommandList(CommandListForAction.ToSharedRef());
						bPopCommandList = true;
					}
					else
					{
						UE_LOG(LogToolMenus, Verbose, TEXT("UI command not found for toolbar entry: %s, toolbar: %s"), *Block.Name.ToString(), *MenuData->MenuName.ToString());
					}

					ToolBarBuilder.AddToolBarButton(Block.Command, Block.Name, Block.Label, Block.ToolTip, Block.Icon, Block.TutorialHighlightName);

					if (bPopCommandList)
					{
						ToolBarBuilder.PopCommandList();
					}
				}
				else if (Block.ScriptObject)
				{
					UToolMenuEntryScript* ScriptObject = Block.ScriptObject;
					TAttribute<FSlateIcon> Icon = ScriptObject->CreateIconAttribute(MenuData->Context);
					ToolBarBuilder.AddToolBarButton(UIAction, ScriptObject->Data.Name, ScriptObject->CreateLabelAttribute(MenuData->Context), ScriptObject->CreateToolTipAttribute(MenuData->Context), Icon, Block.UserInterfaceActionType, Block.TutorialHighlightName);
				}
				else
				{
					ToolBarBuilder.AddToolBarButton(UIAction, Block.Name, Block.Label, Block.ToolTip, Block.Icon, Block.UserInterfaceActionType, Block.TutorialHighlightName);
				}
			}
			else if (Block.Type == EMultiBlockType::ToolBarComboButton)
			{
				FOnGetContent OnGetContent = ConvertWidgetChoice(Block.ToolBarData.ComboButtonContextMenuGenerator, MenuData->Context);
				if (OnGetContent.IsBound())
				{
					ToolBarBuilder.AddComboButton(UIAction, OnGetContent, Block.Label, Block.ToolTip, Block.Icon, Block.ToolBarData.bSimpleComboBox, Block.TutorialHighlightName);
				}
				else
				{
					FName SubMenuFullName = JoinMenuPaths(MenuData->MenuName, Block.Name);
					FOnGetContent Delegate = FOnGetContent::CreateUObject(this, &UToolMenus::GenerateToolbarComboButtonMenu, TWeakObjectPtr<UToolMenu>(MenuData), Block.Name);
					ToolBarBuilder.AddComboButton(UIAction, Delegate, Block.Label, Block.ToolTip, Block.Icon, Block.ToolBarData.bSimpleComboBox, Block.TutorialHighlightName);
				}
			}
			else if (Block.Type == EMultiBlockType::ToolBarSeparator)
			{
				ToolBarBuilder.AddSeparator(Block.Name);
			}
			else if (Block.Type == EMultiBlockType::Widget)
			{
				ToolBarBuilder.AddWidget(Widget.ToSharedRef(), Block.TutorialHighlightName, Block.WidgetData.bSearchable);
			}
			else
			{
				UE_LOG(LogToolMenus, Warning, TEXT("Toolbar '%s', item '%s', type not currently supported: %d"), *MenuData->MenuName.ToString(), *Block.Name.ToString(), Block.Type);
			}
		}

		ToolBarBuilder.EndSection();
	}

	AddReferencedContextObjects(ToolBarBuilder.GetMultiBox(), MenuData);
}

void UToolMenus::PopulateMenuBarBuilder(FMenuBarBuilder& MenuBarBuilder, UToolMenu* MenuData)
{
	for (int i=0; i < MenuData->Sections.Num(); ++i)
	{
		const FToolMenuSection& Section = MenuData->Sections[i];
		for (const FToolMenuEntry& Block : Section.Blocks)
		{
			FName SubMenuFullName = JoinMenuPaths(MenuData->MenuName, Block.Name);
			FNewMenuDelegate NewMenuDelegate;
			if (Block.SubMenuData.ConstructMenu.NewMenuDelegate.IsBound())
			{
				NewMenuDelegate = Block.SubMenuData.ConstructMenu.NewMenuDelegate;
			}
			else
			{
				NewMenuDelegate = FNewMenuDelegate::CreateUObject(this, &UToolMenus::PopulateSubMenu, TWeakObjectPtr<UToolMenu>(MenuData), Block.Name);
			}

			MenuBarBuilder.AddPullDownMenu(
				Block.Label.Get(),
				Block.ToolTip.Get(),
				NewMenuDelegate,
				Block.Name
			);
		}
	}

	AddReferencedContextObjects(MenuBarBuilder.GetMultiBox(), MenuData);
}

FOnGetContent UToolMenus::ConvertWidgetChoice(const FNewToolMenuWidgetChoice& Choice, const FToolMenuContext& Context) const
{
	if (Choice.NewToolMenuWidget.IsBound())
	{
		return FOnGetContent::CreateLambda([ToCall = Choice.NewToolMenuWidget, Context]()
		{
			if (ToCall.IsBound())
			{
				return ToCall.Execute(Context);
			}

			return SNullWidget::NullWidget;
		});
	}
	else if (Choice.NewToolMenu.IsBound())
	{
		return FOnGetContent::CreateLambda([ToCall = Choice.NewToolMenu, Context]()
		{
			if (ToCall.IsBound())
			{
				UToolMenu* MenuData = NewObject<UToolMenu>();
				MenuData->Context = Context;
				ToCall.Execute(MenuData);
				return UToolMenus::Get()->GenerateWidget(MenuData);
			}

			return SNullWidget::NullWidget;
		});
	}
	return Choice.OnGetContent;
}

FUIAction UToolMenus::ConvertUIAction(const FToolMenuEntry& Block, const FToolMenuContext& Context)
{
	FUIAction UIAction;
	
	if (Block.ScriptObject)
	{
		UIAction = ConvertScriptObjectToUIAction(Block.ScriptObject, Context);
	}
	else
	{
		UIAction = ConvertUIAction(Block.Action, Context);
	}
	
	if (!UIAction.ExecuteAction.IsBound() && Block.StringExecuteAction.IsBound())
	{
		UIAction.ExecuteAction = Block.StringExecuteAction.ToExecuteAction(Context);
	}

	return UIAction;
}

FUIAction UToolMenus::ConvertUIAction(const FToolUIActionChoice& Choice, const FToolMenuContext& Context)
{
	if (const FToolUIAction* ToolAction = Choice.GetToolUIAction())
	{
		return ConvertUIAction(*ToolAction, Context);
	}
	else if (const FToolDynamicUIAction* DynamicToolAction = Choice.GetToolDynamicUIAction())
	{
		return ConvertUIAction(*DynamicToolAction, Context);
	}
	else if (const FUIAction* Action = Choice.GetUIAction())
	{
		return *Action;
	}

	return FUIAction();
}

FUIAction UToolMenus::ConvertUIAction(const FToolUIAction& Actions, const FToolMenuContext& Context)
{
	FUIAction UIAction;

	if (Actions.ExecuteAction.IsBound())
	{
		UIAction.ExecuteAction.BindLambda([DelegateToCall = Actions.ExecuteAction, Context]()
		{
			DelegateToCall.ExecuteIfBound(Context);
		});
	}

	if (Actions.CanExecuteAction.IsBound())
	{
		UIAction.CanExecuteAction.BindLambda([DelegateToCall = Actions.CanExecuteAction, Context]()
		{
			return DelegateToCall.Execute(Context);
		});
	}

	if (Actions.GetActionCheckState.IsBound())
	{
		UIAction.GetActionCheckState.BindLambda([DelegateToCall = Actions.GetActionCheckState, Context]()
		{
			return DelegateToCall.Execute(Context);
		});
	}

	if (Actions.IsActionVisibleDelegate.IsBound())
	{
		UIAction.IsActionVisibleDelegate.BindLambda([DelegateToCall = Actions.IsActionVisibleDelegate, Context]()
		{
			return DelegateToCall.Execute(Context);
		});
	}

	return UIAction;
}

FUIAction UToolMenus::ConvertUIAction(const FToolDynamicUIAction& Actions, const FToolMenuContext& Context)
{
	FUIAction UIAction;

	if (Actions.ExecuteAction.IsBound())
	{
		UIAction.ExecuteAction.BindLambda([DelegateToCall = Actions.ExecuteAction, Context]()
		{
			DelegateToCall.ExecuteIfBound(Context);
		});
	}

	if (Actions.CanExecuteAction.IsBound())
	{
		UIAction.CanExecuteAction.BindLambda([DelegateToCall = Actions.CanExecuteAction, Context]()
		{
			return DelegateToCall.Execute(Context);
		});
	}

	if (Actions.GetActionCheckState.IsBound())
	{
		UIAction.GetActionCheckState.BindLambda([DelegateToCall = Actions.GetActionCheckState, Context]()
		{
			return DelegateToCall.Execute(Context);
		});
	}

	if (Actions.IsActionVisibleDelegate.IsBound())
	{
		UIAction.IsActionVisibleDelegate.BindLambda([DelegateToCall = Actions.IsActionVisibleDelegate, Context]()
		{
			return DelegateToCall.Execute(Context);
		});
	}

	return UIAction;
}

FUIAction UToolMenus::ConvertScriptObjectToUIAction(UToolMenuEntryScript* ScriptObject, const FToolMenuContext& Context)
{
	FUIAction UIAction;

	if (ScriptObject)
	{
		UClass* ScriptClass = ScriptObject->GetClass();

		static const FName ExecuteName = GET_FUNCTION_NAME_CHECKED(UToolMenuEntryScript, Execute);
		if (ScriptClass->IsFunctionImplementedInScript(ExecuteName))
		{
			UIAction.ExecuteAction.BindUFunction(ScriptObject, ExecuteName, Context);
		}

		static const FName CanExecuteName = GET_FUNCTION_NAME_CHECKED(UToolMenuEntryScript, CanExecute);
		if (ScriptClass->IsFunctionImplementedInScript(CanExecuteName))
		{
			UIAction.CanExecuteAction.BindUFunction(ScriptObject, CanExecuteName, Context);
		}

		static const FName GetCheckStateName = GET_FUNCTION_NAME_CHECKED(UToolMenuEntryScript, GetCheckState);
		if (ScriptClass->IsFunctionImplementedInScript(GetCheckStateName))
		{
			UIAction.GetActionCheckState.BindUFunction(ScriptObject, GetCheckStateName, Context);
		}

		static const FName IsVisibleName = GET_FUNCTION_NAME_CHECKED(UToolMenuEntryScript, IsVisible);
		if (ScriptClass->IsFunctionImplementedInScript(IsVisibleName))
		{
			UIAction.IsActionVisibleDelegate.BindUFunction(ScriptObject, IsVisibleName, Context);
		}
	}

	return UIAction;
}

void UToolMenus::ExecuteStringCommand(const FToolMenuStringCommand StringCommand, const FToolMenuContext Context)
{
	if (StringCommand.IsBound())
	{
		const FName TypeName = StringCommand.GetTypeName();
		UToolMenus* ToolMenus = UToolMenus::Get();
		if (const FToolMenuExecuteString* Handler = ToolMenus->StringCommandHandlers.Find(TypeName))
		{
			if (Handler->IsBound())
			{
				Handler->Execute(StringCommand.String, Context);
			}
		}
		else
		{
			UE_LOG(LogToolMenus, Warning, TEXT("Unknown string command handler type: '%s'"), *TypeName.ToString());
		}
	}
}

UToolMenu* UToolMenus::FindSubMenuToGenerateWith(const FName InParentName, const FName InChildName)
{
	FName BaseName = InParentName;
	while (BaseName != NAME_None)
	{
		FName JoinedName = JoinMenuPaths(BaseName, InChildName);
		if (UToolMenu* Found = FindMenu(JoinedName))
		{
			return Found;
		}

		UToolMenu* BaseData = FindMenu(BaseName);
		BaseName = BaseData ? BaseData->MenuParent : NAME_None;
	}

	return nullptr;
}

UObject* UToolMenus::FindContext(const FToolMenuContext& InContext, UClass* InClass)
{
	return InContext.FindByClass(InClass);
}

void UToolMenus::AddReferencedContextObjects(const TSharedRef<FMultiBox>& InMultiBox, const UToolMenu* InMenu)
{
	if (InMenu)
	{
		TArray<const UObject*>& References = WidgetObjectReferences.FindOrAdd(InMultiBox);
		References.AddUnique(InMenu);
		for (const TWeakObjectPtr<UObject>& WeakObject : InMenu->Context.ContextObjects)
		{
			if (UObject* Object = WeakObject.Get())
			{
				References.AddUnique(Object);
			}
		}
	}
}

void UToolMenus::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	UToolMenus* This = CastChecked<UToolMenus>(InThis);

	for (auto It = This->WidgetObjectReferences.CreateIterator(); It; ++It)
	{
		if (It->Key.IsValid())
		{
			Collector.AddReferencedObjects(It->Value, InThis);
		}
		else
		{
			It.RemoveCurrent();
		}
	}

	for (auto WidgetsForMenuNameIt = This->GeneratedMenuWidgets.CreateIterator(); WidgetsForMenuNameIt; ++WidgetsForMenuNameIt)
	{
		FGeneratedToolMenuWidgets& WidgetsForMenuName = WidgetsForMenuNameIt->Value;

		for (auto Instance = WidgetsForMenuName.Instances.CreateIterator(); Instance; ++Instance)
		{
			if (Instance->Widget.IsValid())
			{
				Collector.AddReferencedObject(Instance->GeneratedMenu, InThis);
			}
			else
			{
				Instance.RemoveCurrent();
			}
		}

		if (WidgetsForMenuName.Instances.Num() == 0)
		{
			WidgetsForMenuNameIt.RemoveCurrent();
		}
	}

	Super::AddReferencedObjects(InThis, Collector);
}

UToolMenu* UToolMenus::GenerateMenuOrSubMenuForEdit(const UToolMenu* InMenu)
{
	// Make copy of context so we can set bIsEditing flag on it
	FToolMenuContext NewMenuContext = InMenu->Context;
	NewMenuContext.bIsEditing = true;

	if (!InMenu->SubMenuParent)
	{
		return GenerateMenu(InMenu->GetMenuName(), NewMenuContext);
	}

	// Generate each menu leading up to the final submenu because sub-menus are not required to be registered
	TArray<const UToolMenu*> SubMenuChain = InMenu->GetSubMenuChain();
	if (SubMenuChain.Num() > 0)
	{
		UToolMenu* CurrentGeneratedMenu = GenerateMenu(SubMenuChain[0]->GetMenuName(), NewMenuContext);
		for (int32 i=1; i < SubMenuChain.Num(); ++i)
		{
			if (UToolMenu* Menu = GenerateSubMenu(CurrentGeneratedMenu, SubMenuChain[i]->SubMenuSourceEntryName))
			{
				CurrentGeneratedMenu = Menu;
			}
			else
			{
				return nullptr;
			}
		}

		return CurrentGeneratedMenu;
	}

	return nullptr;
}

void UToolMenus::AddMenuSubstitutionDuringGenerate(const FName OriginalMenu, const FName NewMenu)
{
	MenuSubstitutionsDuringGenerate.Add(OriginalMenu, NewMenu);
}

void UToolMenus::RemoveSubstitutionDuringGenerate(const FName InMenu)
{
	if (const FName* FoundOverrideMenuName = MenuSubstitutionsDuringGenerate.Find(InMenu))
	{
		const FName OverrideMenuName = *FoundOverrideMenuName;

		// Update all active widget instances of this menu
		FGeneratedToolMenuWidgets* OverrideMenuWidgets = GeneratedMenuWidgets.Find(OverrideMenuName);
		if (OverrideMenuWidgets)
		{
			FGeneratedToolMenuWidgets* DestMenuWidgets = GeneratedMenuWidgets.Find(InMenu);
			if (DestMenuWidgets)
			{
				DestMenuWidgets->Instances.Append(OverrideMenuWidgets->Instances);
			}
			else
			{
				GeneratedMenuWidgets.Add(InMenu, *OverrideMenuWidgets);
			}

			GeneratedMenuWidgets.Remove(OverrideMenuName);
		}

		MenuSubstitutionsDuringGenerate.Remove(InMenu);

		CleanupStaleWidgetsNextTick();
	}
}

UToolMenu* UToolMenus::GenerateMenu(const FName Name, const FToolMenuContext& InMenuContext)
{
	// Allow substituting one menu for another
	FName MenuName = Name;
	if (const FName* Found = MenuSubstitutionsDuringGenerate.Find(Name))
	{
		MenuName = *Found;
	}

	return GenerateMenuFromHierarchy(CollectHierarchy(MenuName), InMenuContext);
}

UToolMenu* UToolMenus::GenerateMenuFromHierarchy(const TArray<UToolMenu*>& Hierarchy, const FToolMenuContext& InMenuContext)
{
	UToolMenu* GeneratedMenu = NewObject<UToolMenu>(this);

	if (Hierarchy.Num() > 0)
	{
		GeneratedMenu->InitGeneratedCopy(Hierarchy[0], Hierarchy.Last()->MenuName, &InMenuContext);
		for (UToolMenu* HiearchyItem : Hierarchy)
		{
			if (HiearchyItem && !HiearchyItem->bExtendersEnabled)
			{
				GeneratedMenu->SetExtendersEnabled(false);
				break;
			}
		}
		AssembleMenuHierarchy(GeneratedMenu, Hierarchy);
	}

	return GeneratedMenu;
}

TSharedRef< class SWidget > UToolMenus::GenerateWidget(const FName InName, const FToolMenuContext& InMenuContext)
{
	UToolMenu* Generated = GenerateMenu(InName, InMenuContext);
	return GenerateWidget(Generated);
}

TSharedRef<SWidget> UToolMenus::GenerateWidget(const TArray<UToolMenu*>& Hierarchy, const FToolMenuContext& InMenuContext)
{
	if (Hierarchy.Num() == 0)
	{
		return SNullWidget::NullWidget;
	}

	UToolMenu* Generated = GenerateMenuFromHierarchy(Hierarchy, InMenuContext);
	return GenerateWidget(Generated);
}

TSharedRef<SWidget> UToolMenus::GenerateWidget(UToolMenu* GeneratedMenu)
{
	CleanupStaleWidgetsNextTick();

	FGeneratedToolMenuWidgets& WidgetsForMenuName = GeneratedMenuWidgets.FindOrAdd(GeneratedMenu->MenuName);

	// Store a copy so that we can call 'Refresh' on menus not in the database
	FGeneratedToolMenuWidget& GeneratedMenuWidget = WidgetsForMenuName.Instances.AddDefaulted_GetRef();
	GeneratedMenuWidget.GeneratedMenu = DuplicateObject<UToolMenu>(GeneratedMenu, this);
	// Copy native properties that serialize does not
	GeneratedMenuWidget.GeneratedMenu->Context = GeneratedMenu->Context;
	GeneratedMenuWidget.GeneratedMenu->StyleSet = GeneratedMenu->StyleSet;

	if (GeneratedMenu->IsEditing())
	{
		// Convert toolbar into menu during editing
		if (GeneratedMenu->MenuType == EMultiBoxType::ToolBar || GeneratedMenu->MenuType == EMultiBoxType::VerticalToolBar)
		{
			for (FToolMenuSection& Section : GeneratedMenu->Sections)
			{
				for (FToolMenuEntry& Entry : Section.Blocks)
				{
					ModifyEntryForEditDialog(Entry);
				}
			}
		}

		FMenuBuilder MenuBuilder(GeneratedMenu->bShouldCloseWindowAfterMenuSelection, GeneratedMenu->Context.CommandList, GeneratedMenu->Context.GetAllExtenders(), GeneratedMenu->bCloseSelfOnly, GeneratedMenu->StyleSet, GeneratedMenu->bSearchable, GeneratedMenu->MenuName);
		MenuBuilder.SetExtendersEnabled(GeneratedMenu->bExtendersEnabled);
		MenuBuilder.GetMultiBox()->WeakToolMenu = GeneratedMenu;
		PopulateMenuBuilder(MenuBuilder, GeneratedMenu);
		if (GeneratedMenu->ModifyBlockWidgetAfterMake.IsBound())
		{
			MenuBuilder.GetMultiBox()->ModifyBlockWidgetAfterMake = GeneratedMenu->ModifyBlockWidgetAfterMake;
		}
		TSharedRef<SWidget> Result = MenuBuilder.MakeWidget();
		GeneratedMenuWidget.Widget = Result;
		return Result;
	}
	else if (GeneratedMenu->MenuType == EMultiBoxType::Menu)
	{
		FMenuBuilder MenuBuilder(GeneratedMenu->bShouldCloseWindowAfterMenuSelection, GeneratedMenu->Context.CommandList, GeneratedMenu->Context.GetAllExtenders(), GeneratedMenu->bCloseSelfOnly, GeneratedMenu->StyleSet, GeneratedMenu->bSearchable, GeneratedMenu->MenuName);
		MenuBuilder.SetExtendersEnabled(GeneratedMenu->bExtendersEnabled);
		PopulateMenuBuilder(MenuBuilder, GeneratedMenu);
		TSharedRef<SWidget> Result = MenuBuilder.MakeWidget(nullptr, GeneratedMenu->MaxHeight);
		GeneratedMenuWidget.Widget = Result;
		return Result;
	}
	else if (GeneratedMenu->MenuType == EMultiBoxType::MenuBar)
	{
		FMenuBarBuilder MenuBarBuilder(GeneratedMenu->Context.CommandList, GeneratedMenu->Context.GetAllExtenders(), GeneratedMenu->StyleSet, GeneratedMenu->MenuName);
		MenuBarBuilder.SetExtendersEnabled(GeneratedMenu->bExtendersEnabled);
		PopulateMenuBarBuilder(MenuBarBuilder, GeneratedMenu);
		TSharedRef<SWidget> Result = MenuBarBuilder.MakeWidget();
		GeneratedMenuWidget.Widget = Result;
		return Result;
	}
	else if (GeneratedMenu->MenuType == EMultiBoxType::ToolBar || GeneratedMenu->MenuType == EMultiBoxType::VerticalToolBar)
	{
		const EOrientation ToolBarOrientation = (GeneratedMenu->MenuType == EMultiBoxType::VerticalToolBar) ? Orient_Vertical : Orient_Horizontal;
		FToolBarBuilder ToolbarBuilder(GeneratedMenu->Context.CommandList, GeneratedMenu->MenuName, GeneratedMenu->Context.GetAllExtenders(), ToolBarOrientation, GeneratedMenu->bToolBarForceSmallIcons);
		ToolbarBuilder.SetExtendersEnabled(GeneratedMenu->bExtendersEnabled);
		ToolbarBuilder.SetIsFocusable(GeneratedMenu->bToolBarIsFocusable);
		PopulateToolBarBuilder(ToolbarBuilder, GeneratedMenu);
		TSharedRef<SWidget> Result = ToolbarBuilder.MakeWidget();
		GeneratedMenuWidget.Widget = Result;
		return Result;
	}

	return SNullWidget::NullWidget;
}

void UToolMenus::ModifyEntryForEditDialog(FToolMenuEntry& Entry)
{
	if (Entry.Type == EMultiBlockType::ToolBarButton)
	{
		Entry.Type = EMultiBlockType::MenuEntry;
	}
	else if (Entry.Type == EMultiBlockType::ToolBarComboButton)
	{
		Entry.Type = EMultiBlockType::MenuEntry;
		if (Entry.ToolBarData.bSimpleComboBox)
		{
			Entry.SubMenuData.bIsSubMenu = true;
		}
	}
	else if (Entry.Type == EMultiBlockType::ToolBarSeparator)
	{
		Entry.Type = EMultiBlockType::MenuSeparator;
	}
}

void UToolMenus::AssignSetTimerForNextTickDelegate(const FSimpleDelegate& InDelegate)
{
	SetTimerForNextTickDelegate = InDelegate;
}

void UToolMenus::SetNextTickTimer()
{
	if (!bNextTickTimerIsSet)
	{
		if (SetTimerForNextTickDelegate.IsBound())
		{
			bNextTickTimerIsSet = true;
			SetTimerForNextTickDelegate.Execute();
		}
	}
}

void UToolMenus::CleanupStaleWidgetsNextTick()
{
	bCleanupStaleWidgetsNextTick = true;
	SetNextTickTimer();
}

void UToolMenus::RefreshAllWidgets()
{
	bRefreshWidgetsNextTick = true;
	SetNextTickTimer();
}

void UToolMenus::HandleNextTick()
{
	if (bCleanupStaleWidgetsNextTick || bRefreshWidgetsNextTick)
	{
		CleanupStaleWidgets();
		bCleanupStaleWidgetsNextTick = false;

		if (bRefreshWidgetsNextTick)
		{
			for (auto WidgetsForMenuNameIt = GeneratedMenuWidgets.CreateIterator(); WidgetsForMenuNameIt; ++WidgetsForMenuNameIt)
			{
				FGeneratedToolMenuWidgets& WidgetsForMenuName = WidgetsForMenuNameIt->Value;
				for (auto Instance = WidgetsForMenuName.Instances.CreateIterator(); Instance; ++Instance)
				{
					if (Instance->Widget.IsValid())
					{
						RefreshMenuWidget(WidgetsForMenuNameIt->Key, *Instance);
					}
				}
			}

			bRefreshWidgetsNextTick = false;
		}
	}

	bNextTickTimerIsSet = false;
}

void UToolMenus::CleanupStaleWidgets()
{
	for (auto WidgetsForMenuNameIt = GeneratedMenuWidgets.CreateIterator(); WidgetsForMenuNameIt; ++WidgetsForMenuNameIt)
	{
		FGeneratedToolMenuWidgets& WidgetsForMenuName = WidgetsForMenuNameIt->Value;

		for (auto Instance = WidgetsForMenuName.Instances.CreateIterator(); Instance; ++Instance)
		{
			if (!Instance->Widget.IsValid())
			{
				Instance.RemoveCurrent();
			}
		}

		if (WidgetsForMenuName.Instances.Num() == 0)
		{
			WidgetsForMenuNameIt.RemoveCurrent();
		}
	}
}

bool UToolMenus::RefreshMenuWidget(const FName InName)
{
	if (FGeneratedToolMenuWidgets* WidgetsForMenuName = GeneratedMenuWidgets.Find(InName))
	{
		for (auto Instance = WidgetsForMenuName->Instances.CreateIterator(); Instance; ++Instance)
		{
			if (RefreshMenuWidget(InName, *Instance))
			{
				return true;
			}
			else
			{
				Instance.RemoveCurrent();
			}
		}
	}

	return false;
}

bool UToolMenus::RefreshMenuWidget(const FName InName, FGeneratedToolMenuWidget& GeneratedMenuWidget)
{
	if (!GeneratedMenuWidget.Widget.IsValid())
	{
		return false;
	}

	// Regenerate menu from database
	UToolMenu* GeneratedMenu = GenerateMenu(InName, GeneratedMenuWidget.GeneratedMenu->Context);
	GeneratedMenuWidget.GeneratedMenu = GeneratedMenu;

	// Regenerate Multibox
	TSharedRef<SMultiBoxWidget> MultiBoxWidget = StaticCastSharedRef<SMultiBoxWidget>(GeneratedMenuWidget.Widget.Pin().ToSharedRef());
	if (GeneratedMenu->MenuType == EMultiBoxType::Menu)
	{
		FMenuBuilder MenuBuilder(GeneratedMenu->bShouldCloseWindowAfterMenuSelection, GeneratedMenu->Context.CommandList, GeneratedMenu->Context.GetAllExtenders(), GeneratedMenu->bCloseSelfOnly, GeneratedMenu->StyleSet, GeneratedMenu->bSearchable);
		MenuBuilder.SetExtendersEnabled(GeneratedMenu->bExtendersEnabled);
		PopulateMenuBuilder(MenuBuilder, GeneratedMenu);
		MultiBoxWidget->SetMultiBox(MenuBuilder.GetMultiBox());
	}
	else if (GeneratedMenu->MenuType == EMultiBoxType::MenuBar)
	{
		FMenuBarBuilder MenuBarBuilder(GeneratedMenu->Context.CommandList, GeneratedMenu->Context.GetAllExtenders(), GeneratedMenu->StyleSet);
		MenuBarBuilder.SetExtendersEnabled(GeneratedMenu->bExtendersEnabled);
		PopulateMenuBarBuilder(MenuBarBuilder, GeneratedMenu);
		MultiBoxWidget->SetMultiBox(MenuBarBuilder.GetMultiBox());
	}
	else if (GeneratedMenu->MenuType == EMultiBoxType::ToolBar || GeneratedMenu->MenuType == EMultiBoxType::VerticalToolBar)
	{
		const EOrientation ToolBarOrientation = (GeneratedMenu->MenuType == EMultiBoxType::VerticalToolBar) ? Orient_Vertical : Orient_Horizontal;
		FToolBarBuilder ToolbarBuilder(GeneratedMenu->Context.CommandList, GeneratedMenu->MenuName, GeneratedMenu->Context.GetAllExtenders(), ToolBarOrientation, GeneratedMenu->bToolBarForceSmallIcons);
		ToolbarBuilder.SetExtendersEnabled(GeneratedMenu->bExtendersEnabled);
		ToolbarBuilder.SetIsFocusable(GeneratedMenu->bToolBarIsFocusable);
		PopulateToolBarBuilder(ToolbarBuilder, GeneratedMenu);
		MultiBoxWidget->SetMultiBox(ToolbarBuilder.GetMultiBox());
	}

	MultiBoxWidget->BuildMultiBoxWidget();
	return true;
}

UToolMenu* UToolMenus::GenerateMenuAsBuilder(const UToolMenu* InMenu, const FToolMenuContext& InMenuContext)
{
	TArray<UToolMenu*> Hierarchy = CollectHierarchy(InMenu->MenuName);

	// Insert InMenu as second to last so items in InMenu appear before items registered in database by other plugins
	if (Hierarchy.Num() > 0)
	{
		Hierarchy.Insert((UToolMenu*)InMenu, Hierarchy.Num() - 1);
	}
	else
	{
		Hierarchy.Add((UToolMenu*)InMenu);
	}

	return GenerateMenuFromHierarchy(Hierarchy, InMenuContext);
}

UToolMenu* UToolMenus::RegisterMenu(const FName InName, const FName InParent, EMultiBoxType InType, bool bWarnIfAlreadyRegistered)
{
	if (UToolMenu* Found = FindMenu(InName))
	{
		if (!Found->bRegistered)
		{
			Found->MenuParent = InParent;
			Found->MenuType = InType;
			Found->MenuOwner = CurrentOwner();
			Found->bRegistered = true;
			Found->bIsRegistering = true;
			for (FToolMenuSection& Section : Found->Sections)
			{
				Section.bIsRegistering = Found->bIsRegistering;
			}
		}
		else if (bWarnIfAlreadyRegistered)
		{
			UE_LOG(LogToolMenus, Warning, TEXT("Menu already registered : %s"), *InName.ToString());
		}

		return Found;
	}

	UToolMenu* ToolMenu = NewObject<UToolMenu>(this);
	ToolMenu->InitMenu(CurrentOwner(), InName, InParent, InType);
	ToolMenu->bRegistered = true;
	ToolMenu->bIsRegistering = true;
	Menus.Add(InName, ToolMenu);
	return ToolMenu;
}

UToolMenu* UToolMenus::ExtendMenu(const FName InName)
{
	if (UToolMenu* Found = FindMenu(InName))
	{
		Found->bIsRegistering = false;
		for (FToolMenuSection& Section : Found->Sections)
		{
			Section.bIsRegistering = Found->bIsRegistering;
		}
		return Found;
	}

	UToolMenu* ToolMenu = NewObject<UToolMenu>(this);
	ToolMenu->MenuName = InName;
	ToolMenu->bRegistered = false;
	ToolMenu->bIsRegistering = false;
	Menus.Add(InName, ToolMenu);
	return ToolMenu;
}

void UToolMenus::RemoveMenu(const FName MenuName)
{
	Menus.Remove(MenuName);
}

bool UToolMenus::AddMenuEntryObject(UToolMenuEntryScript* MenuEntryObject)
{
	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(MenuEntryObject->Data.Menu);
	Menu->AddMenuEntryObject(MenuEntryObject);
	return true;
}

void UToolMenus::SetSectionLabel(const FName MenuName, const FName SectionName, const FText Label)
{
	ExtendMenu(MenuName)->FindOrAddSection(SectionName).Label = TAttribute<FText>(Label);
}

void UToolMenus::SetSectionPosition(const FName MenuName, const FName SectionName, const FName PositionName, const EToolMenuInsertType PositionType)
{
	ExtendMenu(MenuName)->FindOrAddSection(SectionName).InsertPosition = FToolMenuInsert(PositionName, PositionType);
}

void UToolMenus::AddSection(const FName MenuName, const FName SectionName, const TAttribute< FText >& InLabel, const FToolMenuInsert InPosition)
{
	UToolMenu* Menu = ExtendMenu(MenuName);
	FToolMenuSection* Section = Menu->FindSection(SectionName);
	if (!Section)
	{
		Menu->AddSection(SectionName, InLabel, InPosition);
	}
}

void UToolMenus::RemoveSection(const FName MenuName, const FName InSection)
{
	if (UToolMenu* Menu = FindMenu(MenuName))
	{
		Menu->RemoveSection(InSection);
	}
}

void UToolMenus::AddEntry(const FName MenuName, const FName InSection, const FToolMenuEntry& InEntry)
{
	ExtendMenu(MenuName)->FindOrAddSection(InSection).AddEntry(InEntry);
}

void UToolMenus::RemoveEntry(const FName MenuName, const FName InSection, const FName InName)
{
	if (UToolMenu* Menu = FindMenu(MenuName))
	{
		if (FToolMenuSection* Section = Menu->FindSection(InSection))
		{
			Section->RemoveEntry(InName);
		}
	}
}

void UToolMenus::UnregisterOwnerInternal(FToolMenuOwner InOwner)
{
	if (InOwner != FToolMenuOwner())
	{
		for (auto It = Menus.CreateIterator(); It; ++It)
		{
			int32 NumEntriesRemoved = 0;

			UToolMenu* Menu = It->Value;
			for (FToolMenuSection& Section : Menu->Sections)
			{
				NumEntriesRemoved += Section.RemoveEntriesByOwner(InOwner);
			}

			// Refresh any widgets that are currently displayed to the user
			if (NumEntriesRemoved > 0)
			{
				RefreshAllWidgets();
			}
		}
	}
}

FToolMenuOwner UToolMenus::CurrentOwner() const
{
	if (OwnerStack.Num() > 0)
	{
		return OwnerStack.Last();
	}

	return FToolMenuOwner();
}

void UToolMenus::PushOwner(const FToolMenuOwner InOwner)
{
	OwnerStack.Add(InOwner);
}

void UToolMenus::PopOwner(const FToolMenuOwner InOwner)
{
	FToolMenuOwner PoppedOwner = OwnerStack.Pop(false);
	check(PoppedOwner == InOwner);
}

void UToolMenus::UnregisterOwnerByName(FName InOwnerName)
{
	UnregisterOwnerInternal(InOwnerName);
}

void UToolMenus::RegisterStringCommandHandler(const FName InName, const FToolMenuExecuteString& InDelegate)
{
	StringCommandHandlers.Add(InName, InDelegate);
}

void UToolMenus::UnregisterStringCommandHandler(const FName InName)
{
	StringCommandHandlers.Remove(InName);
}

FDelegateHandle UToolMenus::RegisterStartupCallback(const FSimpleMulticastDelegate::FDelegate& InDelegate)
{
	if (IsToolMenuUIEnabled() && UToolMenus::TryGet())
	{
		// Call immediately if systems are initialized
		InDelegate.Execute();
	}
	else
	{
		// Defer call to occur after systems are initialized (slate and menus)
		FDelegateHandle Result = StartupCallbacks.Add(InDelegate);

		if (!InternalStartupCallbackHandle.IsSet())
		{
			InternalStartupCallbackHandle = FCoreDelegates::OnPostEngineInit.Add(FSimpleMulticastDelegate::FDelegate::CreateStatic(&UToolMenus::PrivateStartupCallback));
		}

		return Result;
	}

	return FDelegateHandle();
}

void UToolMenus::UnRegisterStartupCallback(const void* UserPointer)
{
	StartupCallbacks.RemoveAll(UserPointer);
}

void UToolMenus::UnRegisterStartupCallback(FDelegateHandle InHandle)
{
	StartupCallbacks.Remove(InHandle);
}

void UToolMenus::PrivateStartupCallback()
{
	UnregisterPrivateStartupCallback();

	if (IsToolMenuUIEnabled() && UToolMenus::TryGet())
	{
		StartupCallbacks.Broadcast();
		StartupCallbacks.Clear();
	}
}

void UToolMenus::UnregisterPrivateStartupCallback()
{
	if (InternalStartupCallbackHandle.IsSet())
	{
		FDelegateHandle& Handle = InternalStartupCallbackHandle.GetValue();
		if (Handle.IsValid())
		{
			FCoreDelegates::OnPostEngineInit.Remove(Handle);
			Handle.Reset();
		}
	}
}

void UToolMenus::SaveCustomizations()
{
	SaveConfig();
}

void UToolMenus::RemoveAllCustomizations()
{
	CustomizedMenus.Reset();
}

#undef LOCTEXT_NAMESPACE