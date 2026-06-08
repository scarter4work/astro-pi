/**
 * BayesianAstro Interface Implementation
 */

#include "BayesianAstroInterface.h"
#include "BayesianAstroProcess.h"

#include <pcl/File.h>
#include <pcl/FileDialog.h>

namespace pcl
{

BayesianAstroInterface* TheBayesianAstroInterface = nullptr;

BayesianAstroInterface::BayesianAstroInterface()
    : m_instance(TheBayesianAstroProcess)
{
    TheBayesianAstroInterface = this;
}

BayesianAstroInterface::~BayesianAstroInterface()
{
    TheBayesianAstroInterface = nullptr;
}

IsoString BayesianAstroInterface::Id() const
{
    return "BayesianAstro";
}

MetaProcess* BayesianAstroInterface::Process() const
{
    return TheBayesianAstroProcess;
}

String BayesianAstroInterface::IconImageSVGFile() const
{
    return String();
}

InterfaceFeatures BayesianAstroInterface::Features() const
{
    return InterfaceFeature::Default | InterfaceFeature::ApplyGlobalButton;
}

void BayesianAstroInterface::ApplyInstance() const
{
    m_instance.LaunchGlobal();
}

void BayesianAstroInterface::ResetInstance()
{
    BayesianAstroInstance defaultInstance(TheBayesianAstroProcess);
    ImportProcess(defaultInstance);
}

bool BayesianAstroInterface::Launch(const MetaProcess&, const ProcessImplementation* instance, bool& dynamic, unsigned& flags)
{
    if (instance != nullptr)
        ImportProcess(*instance);

    // Build GUI only once
    if (!m_guiInitialized)
    {
        int labelWidth = Font().Width(String("Confidence Threshold:") + 'M');

        // === Allocate controls ===
        Global_Sizer = new VerticalSizer;

        // Input files section
        InputFiles_SectionBar = new SectionBar;
        InputFiles_Control = new Control;
        InputFiles_Sizer = new HorizontalSizer;
        InputFiles_TreeBox = new TreeBox;
        InputFilesButtons_Sizer = new VerticalSizer;
        AddFiles_ToolButton = new ToolButton;
        RemoveFiles_ToolButton = new ToolButton;
        ClearFiles_ToolButton = new ToolButton;

        // Parameters section
        Parameters_SectionBar = new SectionBar;
        Parameters_Control = new Control;
        Parameters_Sizer = new VerticalSizer;
        FusionStrategy_Sizer = new HorizontalSizer;
        FusionStrategy_Label = new Label;
        FusionStrategy_ComboBox = new ComboBox;
        OutlierSigma_NumericControl = new NumericControl;
        ConfidenceThreshold_NumericControl = new NumericControl;
        Options_Sizer = new HorizontalSizer;
        UseGPU_CheckBox = new CheckBox;
        GenerateConfidenceMap_CheckBox = new CheckBox;

        // Output section
        Output_SectionBar = new SectionBar;
        Output_Control = new Control;
        Output_Sizer = new VerticalSizer;
        OutputDir_Sizer = new HorizontalSizer;
        OutputDir_Label = new Label;
        OutputDir_Edit = new Edit;
        OutputDir_ToolButton = new ToolButton;
        OutputPrefix_Sizer = new HorizontalSizer;
        OutputPrefix_Label = new Label;
        OutputPrefix_Edit = new Edit;

        // === Input Files Section ===

        InputFiles_TreeBox->SetMinHeight(120);
        InputFiles_TreeBox->SetNumberOfColumns(1);
        InputFiles_TreeBox->SetHeaderText(0, "Input Files");
        InputFiles_TreeBox->EnableMultipleSelections();
        InputFiles_TreeBox->DisableRootDecoration();
        InputFiles_TreeBox->EnableAlternateRowColor();
        InputFiles_TreeBox->OnNodeSelectionUpdated((TreeBox::tree_event_handler)&BayesianAstroInterface::e_NodeSelectionUpdated, *this);

        AddFiles_ToolButton->SetIcon(ScaledResource(":/icons/document-open.png"));
        AddFiles_ToolButton->SetScaledFixedSize(22, 22);
        AddFiles_ToolButton->SetToolTip("<p>Add FITS files</p>");
        AddFiles_ToolButton->OnClick((Button::click_event_handler)&BayesianAstroInterface::e_Click, *this);

        RemoveFiles_ToolButton->SetIcon(ScaledResource(":/icons/delete.png"));
        RemoveFiles_ToolButton->SetScaledFixedSize(22, 22);
        RemoveFiles_ToolButton->SetToolTip("<p>Remove selected files</p>");
        RemoveFiles_ToolButton->OnClick((Button::click_event_handler)&BayesianAstroInterface::e_Click, *this);

        ClearFiles_ToolButton->SetIcon(ScaledResource(":/icons/clear.png"));
        ClearFiles_ToolButton->SetScaledFixedSize(22, 22);
        ClearFiles_ToolButton->SetToolTip("<p>Clear all files</p>");
        ClearFiles_ToolButton->OnClick((Button::click_event_handler)&BayesianAstroInterface::e_Click, *this);

        InputFilesButtons_Sizer->SetSpacing(4);
        InputFilesButtons_Sizer->Add(*AddFiles_ToolButton);
        InputFilesButtons_Sizer->Add(*RemoveFiles_ToolButton);
        InputFilesButtons_Sizer->Add(*ClearFiles_ToolButton);
        InputFilesButtons_Sizer->AddStretch();

        InputFiles_Sizer->SetSpacing(4);
        InputFiles_Sizer->Add(*InputFiles_TreeBox, 100);
        InputFiles_Sizer->Add(*InputFilesButtons_Sizer);

        InputFiles_Control->SetSizer(*InputFiles_Sizer);

        InputFiles_SectionBar->SetTitle("Input Files");
        InputFiles_SectionBar->SetSection(*InputFiles_Control);

        // === Parameters Section ===

        FusionStrategy_Label->SetText("Fusion Strategy:");
        FusionStrategy_Label->SetFixedWidth(labelWidth);
        FusionStrategy_Label->SetTextAlignment(TextAlign::Right | TextAlign::VertCenter);

        FusionStrategy_ComboBox->AddItem("MLE (Maximum Likelihood)");
        FusionStrategy_ComboBox->AddItem("Confidence Weighted");
        FusionStrategy_ComboBox->AddItem("Lucky Imaging");
        FusionStrategy_ComboBox->AddItem("Multi-Scale");
        FusionStrategy_ComboBox->OnItemSelected((ComboBox::item_event_handler)&BayesianAstroInterface::e_ItemSelected, *this);

        FusionStrategy_Sizer->SetSpacing(4);
        FusionStrategy_Sizer->Add(*FusionStrategy_Label);
        FusionStrategy_Sizer->Add(*FusionStrategy_ComboBox, 100);

        OutlierSigma_NumericControl->label.SetText("Outlier Sigma:");
        OutlierSigma_NumericControl->label.SetFixedWidth(labelWidth);
        OutlierSigma_NumericControl->slider.SetRange(0, 100);
        OutlierSigma_NumericControl->SetReal();
        OutlierSigma_NumericControl->SetRange(0.5, 10.0);
        OutlierSigma_NumericControl->SetPrecision(2);
        OutlierSigma_NumericControl->OnValueUpdated((NumericEdit::value_event_handler)&BayesianAstroInterface::e_ValueUpdated, *this);

        ConfidenceThreshold_NumericControl->label.SetText("Confidence Threshold:");
        ConfidenceThreshold_NumericControl->label.SetFixedWidth(labelWidth);
        ConfidenceThreshold_NumericControl->slider.SetRange(0, 100);
        ConfidenceThreshold_NumericControl->SetReal();
        ConfidenceThreshold_NumericControl->SetRange(0.0, 1.0);
        ConfidenceThreshold_NumericControl->SetPrecision(2);
        ConfidenceThreshold_NumericControl->OnValueUpdated((NumericEdit::value_event_handler)&BayesianAstroInterface::e_ValueUpdated, *this);

        UseGPU_CheckBox->SetText("Use GPU Acceleration");
        UseGPU_CheckBox->OnClick((Button::click_event_handler)&BayesianAstroInterface::e_Click, *this);

        GenerateConfidenceMap_CheckBox->SetText("Generate Confidence Map");
        GenerateConfidenceMap_CheckBox->OnClick((Button::click_event_handler)&BayesianAstroInterface::e_Click, *this);

        Options_Sizer->SetSpacing(16);
        Options_Sizer->Add(*UseGPU_CheckBox);
        Options_Sizer->Add(*GenerateConfidenceMap_CheckBox);
        Options_Sizer->AddStretch();

        Parameters_Sizer->SetSpacing(4);
        Parameters_Sizer->Add(*FusionStrategy_Sizer);
        Parameters_Sizer->Add(*OutlierSigma_NumericControl);
        Parameters_Sizer->Add(*ConfidenceThreshold_NumericControl);
        Parameters_Sizer->Add(*Options_Sizer);

        Parameters_Control->SetSizer(*Parameters_Sizer);

        Parameters_SectionBar->SetTitle("Processing Parameters");
        Parameters_SectionBar->SetSection(*Parameters_Control);

        // === Output Section ===

        OutputDir_Label->SetText("Output Directory:");
        OutputDir_Label->SetFixedWidth(labelWidth);
        OutputDir_Label->SetTextAlignment(TextAlign::Right | TextAlign::VertCenter);

        OutputDir_Edit->SetReadOnly();
        OutputDir_Edit->OnEditCompleted((Edit::edit_event_handler)&BayesianAstroInterface::e_EditCompleted, *this);

        OutputDir_ToolButton->SetIcon(ScaledResource(":/icons/select-file.png"));
        OutputDir_ToolButton->SetScaledFixedSize(22, 22);
        OutputDir_ToolButton->SetToolTip("<p>Select output directory</p>");
        OutputDir_ToolButton->OnClick((Button::click_event_handler)&BayesianAstroInterface::e_Click, *this);

        OutputDir_Sizer->SetSpacing(4);
        OutputDir_Sizer->Add(*OutputDir_Label);
        OutputDir_Sizer->Add(*OutputDir_Edit, 100);
        OutputDir_Sizer->Add(*OutputDir_ToolButton);

        OutputPrefix_Label->SetText("Output Prefix:");
        OutputPrefix_Label->SetFixedWidth(labelWidth);
        OutputPrefix_Label->SetTextAlignment(TextAlign::Right | TextAlign::VertCenter);

        OutputPrefix_Edit->OnEditCompleted((Edit::edit_event_handler)&BayesianAstroInterface::e_EditCompleted, *this);

        OutputPrefix_Sizer->SetSpacing(4);
        OutputPrefix_Sizer->Add(*OutputPrefix_Label);
        OutputPrefix_Sizer->Add(*OutputPrefix_Edit, 100);

        Output_Sizer->SetSpacing(4);
        Output_Sizer->Add(*OutputDir_Sizer);
        Output_Sizer->Add(*OutputPrefix_Sizer);

        Output_Control->SetSizer(*Output_Sizer);

        Output_SectionBar->SetTitle("Output");
        Output_SectionBar->SetSection(*Output_Control);

        // === Global Layout ===

        Global_Sizer->SetMargin(8);
        Global_Sizer->SetSpacing(6);
        Global_Sizer->Add(*InputFiles_SectionBar);
        Global_Sizer->Add(*InputFiles_Control);
        Global_Sizer->Add(*Parameters_SectionBar);
        Global_Sizer->Add(*Parameters_Control);
        Global_Sizer->Add(*Output_SectionBar);
        Global_Sizer->Add(*Output_Control);

        SetSizer(*Global_Sizer);

        SetScaledMinWidth(500);
        AdjustToContents();

        m_guiInitialized = true;
    }

    UpdateControls();

    dynamic = false;
    return true;
}

ProcessImplementation* BayesianAstroInterface::NewProcess() const
{
    return new BayesianAstroInstance(m_instance);
}

bool BayesianAstroInterface::ValidateProcess(const ProcessImplementation& p, String& whyNot) const
{
    if (dynamic_cast<const BayesianAstroInstance*>(&p) != nullptr)
        return true;
    whyNot = "Not a BayesianAstro instance.";
    return false;
}

bool BayesianAstroInterface::RequiresInstanceValidation() const
{
    return true;
}

bool BayesianAstroInterface::ImportProcess(const ProcessImplementation& p)
{
    const BayesianAstroInstance* instance = dynamic_cast<const BayesianAstroInstance*>(&p);
    if (instance == nullptr)
        return false;

    m_instance = *instance;
    UpdateControls();
    return true;
}

void BayesianAstroInterface::UpdateControls()
{
    if (!m_guiInitialized)
        return;

    FusionStrategy_ComboBox->SetCurrentItem(m_instance.p_fusionStrategy);
    OutlierSigma_NumericControl->SetValue(m_instance.p_outlierSigma);
    ConfidenceThreshold_NumericControl->SetValue(m_instance.p_confidenceThreshold);
    UseGPU_CheckBox->SetChecked(m_instance.p_useGPU);
    GenerateConfidenceMap_CheckBox->SetChecked(m_instance.p_generateConfidenceMap);
    OutputDir_Edit->SetText(m_instance.p_outputDirectory);
    OutputPrefix_Edit->SetText(m_instance.p_outputPrefix);

    UpdateInputFilesList();
}

void BayesianAstroInterface::UpdateInputFilesList()
{
    InputFiles_TreeBox->DisableUpdates();
    InputFiles_TreeBox->Clear();

    for (const String& path : m_instance.p_inputFiles)
    {
        TreeBox::Node* node = new TreeBox::Node(*InputFiles_TreeBox);
        node->SetText(0, File::ExtractNameAndSuffix(path));
        node->SetToolTip(0, path);
    }

    InputFiles_TreeBox->EnableUpdates();
}

void BayesianAstroInterface::e_Click(Button& sender, bool checked)
{
    if (sender == *AddFiles_ToolButton)
    {
        OpenFileDialog dlg;
        dlg.SetCaption("Select FITS Files");
        dlg.EnableMultipleSelections();
        dlg.SetFilter(FileFilter("FITS Files", StringList() << ".fit" << ".fits" << ".fts"));

        if (dlg.Execute())
        {
            for (const String& path : dlg.FileNames())
                m_instance.p_inputFiles.Add(path);
            UpdateInputFilesList();
        }
    }
    else if (sender == *RemoveFiles_ToolButton)
    {
        IndirectArray<TreeBox::Node> selected = InputFiles_TreeBox->SelectedNodes();
        for (int i = selected.Length() - 1; i >= 0; --i)
        {
            int index = InputFiles_TreeBox->ChildIndex(selected[i]);
            if (index >= 0 && index < int(m_instance.p_inputFiles.Length()))
                m_instance.p_inputFiles.Remove(m_instance.p_inputFiles.At(index));
        }
        UpdateInputFilesList();
    }
    else if (sender == *ClearFiles_ToolButton)
    {
        m_instance.p_inputFiles.Clear();
        UpdateInputFilesList();
    }
    else if (sender == *OutputDir_ToolButton)
    {
        GetDirectoryDialog dlg;
        dlg.SetCaption("Select Output Directory");
        if (!m_instance.p_outputDirectory.IsEmpty())
            dlg.SetInitialPath(m_instance.p_outputDirectory);

        if (dlg.Execute())
        {
            m_instance.p_outputDirectory = dlg.Directory();
            OutputDir_Edit->SetText(m_instance.p_outputDirectory);
        }
    }
    else if (sender == *UseGPU_CheckBox)
    {
        m_instance.p_useGPU = checked;
    }
    else if (sender == *GenerateConfidenceMap_CheckBox)
    {
        m_instance.p_generateConfidenceMap = checked;
    }
}

void BayesianAstroInterface::e_ItemSelected(ComboBox& sender, int itemIndex)
{
    if (sender == *FusionStrategy_ComboBox)
    {
        m_instance.p_fusionStrategy = itemIndex;
    }
}

void BayesianAstroInterface::e_ValueUpdated(NumericEdit& sender, double value)
{
    if (&sender == static_cast<NumericEdit*>(OutlierSigma_NumericControl))
    {
        m_instance.p_outlierSigma = value;
    }
    else if (&sender == static_cast<NumericEdit*>(ConfidenceThreshold_NumericControl))
    {
        m_instance.p_confidenceThreshold = value;
    }
}

void BayesianAstroInterface::e_EditCompleted(Edit& sender)
{
    if (sender == *OutputPrefix_Edit)
    {
        m_instance.p_outputPrefix = sender.Text();
    }
}

void BayesianAstroInterface::e_NodeSelectionUpdated(TreeBox& sender)
{
    // Could enable/disable remove button based on selection
}

} // namespace pcl
