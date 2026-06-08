/**
 * BayesianAstro Module
 *
 * PixInsight module for distribution-aware image stacking.
 *
 * Copyright (c) 2025 Scott Carter. All rights reserved.
 */

#ifndef __BayesianAstroModule_h
#define __BayesianAstroModule_h

#include <pcl/MetaModule.h>

namespace pcl
{

class BayesianAstroModule : public MetaModule
{
public:
    BayesianAstroModule();

    const char* Version() const override;
    IsoString Name() const override;
    String Description() const override;
    String Company() const override;
    String Author() const override;
    String Copyright() const override;
    String TradeMarks() const override;
    String OriginalFileName() const override;
    void GetReleaseDate(int& year, int& month, int& day) const override;
};

extern BayesianAstroModule* TheBayesianAstroModule;

} // namespace pcl

#endif // __BayesianAstroModule_h
