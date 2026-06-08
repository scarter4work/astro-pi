/**
 * BayesianAstro Interface
 *
 * Process interface using native PCL controls.
 * GUI elements are created in Launch() as pointers to avoid construction issues.
 */

#ifndef __BayesianAstroInterface_h
#define __BayesianAstroInterface_h

#include <pcl/ProcessInterface.h>
#include <pcl/Sizer.h>
#include <pcl/SectionBar.h>
#include <pcl/ToolButton.h>
#include <pcl/PushButton.h>
#include <pcl/NumericControl.h>
#include <pcl/TreeBox.h>
#include <pcl/ComboBox.h>
#include <pcl/CheckBox.h>
#include <pcl/Label.h>
#include <pcl/Edit.h>
#include <pcl/GroupBox.h>

#include "BayesianAstroInstance.h"

namespace pcl
{

class BayesianAstroInterface : public ProcessInterface
{
public:
    BayesianAstroInterface();
    virtual ~BayesianAstroInterface();

    IsoString Id() const override;
    MetaProcess* Process() const override;
    String IconImageSVGFile() const override;
    InterfaceFeatures Features() const override;
    void ApplyInstance() const override;
    void ResetInstance() override;
    bool Launch(const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& flags) override;
    ProcessImplementation* NewProcess() const override;
    bool ValidateProcess(const ProcessImplementation&, String& whyNot) const override;
    bool RequiresInstanceValidation() const override;
    bool ImportProcess(const ProcessImplementation&) override;

private:
    BayesianAstroInstance m_instance;
    bool m_guiInitialized = false;

    // GUI elements - pointers created in Launch()
    VerticalSizer*      Global_Sizer = nullptr;

    // Input files section
    SectionBar*         InputFiles_SectionBar = nullptr;
    Control*            InputFiles_Control = nullptr;
    HorizontalSizer*    InputFiles_Sizer = nullptr;
    TreeBox*            InputFiles_TreeBox = nullptr;
    VerticalSizer*      InputFilesButtons_Sizer = nullptr;
    ToolButton*         AddFiles_ToolButton = nullptr;
    ToolButton*         RemoveFiles_ToolButton = nullptr;
    ToolButton*         ClearFiles_ToolButton = nullptr;

    // Processing parameters section
    SectionBar*         Parameters_SectionBar = nullptr;
    Control*            Parameters_Control = nullptr;
    VerticalSizer*      Parameters_Sizer = nullptr;

    HorizontalSizer*    FusionStrategy_Sizer = nullptr;
    Label*              FusionStrategy_Label = nullptr;
    ComboBox*           FusionStrategy_ComboBox = nullptr;

    NumericControl*     OutlierSigma_NumericControl = nullptr;
    NumericControl*     ConfidenceThreshold_NumericControl = nullptr;

    HorizontalSizer*    Options_Sizer = nullptr;
    CheckBox*           UseGPU_CheckBox = nullptr;
    CheckBox*           GenerateConfidenceMap_CheckBox = nullptr;

    // Output section
    SectionBar*         Output_SectionBar = nullptr;
    Control*            Output_Control = nullptr;
    VerticalSizer*      Output_Sizer = nullptr;

    HorizontalSizer*    OutputDir_Sizer = nullptr;
    Label*              OutputDir_Label = nullptr;
    Edit*               OutputDir_Edit = nullptr;
    ToolButton*         OutputDir_ToolButton = nullptr;

    HorizontalSizer*    OutputPrefix_Sizer = nullptr;
    Label*              OutputPrefix_Label = nullptr;
    Edit*               OutputPrefix_Edit = nullptr;

    // Event handlers
    void e_Click(Button& sender, bool checked);
    void e_ItemSelected(ComboBox& sender, int itemIndex);
    void e_ValueUpdated(NumericEdit& sender, double value);
    void e_EditCompleted(Edit& sender);
    void e_NodeSelectionUpdated(TreeBox& sender);

    // Helper methods
    void UpdateControls();
    void UpdateInputFilesList();
};

extern BayesianAstroInterface* TheBayesianAstroInterface;

} // namespace pcl

#endif // __BayesianAstroInterface_h
