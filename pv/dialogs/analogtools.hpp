// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PV_DIALOGS_ANALOGTOOLS_HPP
#define PV_DIALOGS_ANALOGTOOLS_HPP

#include <QDialog>
#include <memory>

namespace pv
{
class Session;
namespace data
{
class MathSignal;
}
namespace dialogs
{
void edit_analog_processing(Session &session, QWidget *parent,
							std::shared_ptr<data::MathSignal> signal = {});
QDialog *create_analog_view(Session &session, QWidget *parent, bool spectrum);
} // namespace dialogs
} // namespace pv
#endif
