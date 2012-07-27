/*
 * module-ews-mail-config.c
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the program; if not, see <http://www.gnu.org/licenses/>
 *
 */

#include "e-cal-config-ews.h"
#include "e-book-config-ews.h"
#include "e-mail-config-ews-autodiscover.h"
#include "e-mail-config-ews-backend.h"
#include "e-mail-config-ews-gal.h"
#include "e-mail-config-ews-notebook.h"
#include "e-mail-config-ews-oal-combo-box.h"
#include "e-mail-config-ews-ooo-page.h"

/* Module Entry Points */
void e_module_load (GTypeModule *type_module);
void e_module_unload (GTypeModule *type_module);

G_MODULE_EXPORT void
e_module_load (GTypeModule *type_module)
{
	e_cal_config_ews_type_register (type_module);
	e_book_config_ews_type_register (type_module);
	e_mail_config_ews_autodiscover_type_register (type_module);
	e_mail_config_ews_backend_type_register (type_module);
	e_mail_config_ews_gal_type_register (type_module);
	e_mail_config_ews_notebook_type_register (type_module);
	e_mail_config_ews_oal_combo_box_type_register (type_module);
	e_mail_config_ews_ooo_page_type_register (type_module);
}

G_MODULE_EXPORT void
e_module_unload (GTypeModule *type_module)
{
}
