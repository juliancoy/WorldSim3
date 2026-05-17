# Data Sources

This file is the practical inventory for data the app pulls or tracks. It is not a taxonomy document.

## Explicit URLs

These are the URLs currently referenced by the layer manifests.

### Geometry MDP sources

These are the Maryland Planning geometry sources that the regional parcel build prefers first.

- `https://planning.maryland.gov/Pages/OurProducts/DownloadFiles.aspx` - Maryland Planning parcel download portal
- `https://planning.maryland.gov/pages/ourproducts/downloadfiles.aspx` - Same portal, lowercase variant used in manifests
- `https://planning.maryland.gov/MSDC/Pages/91_property_mapping/parcel-data.aspx` - Maryland parcel documentation
- `https://mdgeodata.md.gov/imap/rest/services/PlanningCadastre/MD_ParcelBoundaries/MapServer` - Maryland parcel map service
- `https://www.dropbox.com/scl/fi/i08w46iye7mkeb15la9fk/February_2026_Parcels.zip?rlkey=r8y6lzyhj5sqf5mse62vonb4e&dl=1` - Parcel ZIP referenced for Maryland parcel intake

### Core Baltimore operational layers

- `https://data.baltimorecity.gov/api/download/v1/items/64110b108565433d8da40dd0e422064e/geojson?layers=0` - Real Property Information
- `https://data.baltimorecity.gov/api/download/v1/items/85767997c73d4b9292415f2661466273/geojson?layers=0` - Parcel geometry
- `https://data.baltimorecity.gov/api/download/v1/items/dc7bf04cec4e41ef85cc6b391652e1e7/geojson?layers=0` - Zoning
- `https://data.baltimorecity.gov/api/download/v1/items/189e6d1c65df4e13b38c0027cee574f6/geojson?layers=3` - Housing and Building Permits, 2019-present
- `https://data.baltimorecity.gov/api/download/v1/items/691d65a5f85640e6aaa46930bd9dc102/geojson?layers=1` - Vacant Building Notices
- `https://data.baltimorecity.gov/api/download/v1/items/204beefe92a645d79fdf0969957bbdf8/geojson?layers=0` - NIBRS Group A crime
- `https://services1.arcgis.com/UWYHeuuJISiGmgXx/ArcGIS/rest/services/OpenNoticesDashboard/FeatureServer/0` - Open Notices dashboard
- `https://data.baltimorecity.gov/api/download/v1/items/4db6d1e54e714a3e8125990a09d4623d/geojson?layers=2` - Vacant Building Rehabs
- `https://egis.baltimorecity.gov/egis/rest/services/Housing/dmxLandPlanning/MapServer/38` - CodeMap recently rehabbed vacant buildings
- `https://egis.baltimorecity.gov/egis/rest/services/Housing/dmxLandPlanning/MapServer/27` - CodeMap recently rehabbed VBN parcels
- `https://data.baltimorecity.gov/api/download/v1/items/efaf11a277fd417bbc15887fc3d627ca/geojson?layers=0` - Tax lien certificate sale properties
- `https://data.baltimorecity.gov/api/download/v1/items/20ca888a394c4f77b0a9c31668f605b3/geojson?layers=0` - 2021 tax sale list
- `https://data.baltimorecity.gov/api/download/v1/items/1b844b06c28a45ca86cdfb586fdce3a7/geojson?layers=7` - Open Bid List / Vacants to Value
- `https://data.baltimorecity.gov/api/download/v1/items/37f242ca93b244a998c31b6c0a3696a7/geojson?layers=0` - Completed City Demo

### Next parcel-ready additions

- `https://services1.arcgis.com/UWYHeuuJISiGmgXx/arcgis/rest/services/ECB_OB/FeatureServer/0/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - Environmental citations
- `https://geodata.baltimorecity.gov/egis/rest/services/Housing/OpenGov_RegAndLicense/MapServer/0/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - Rental registration and licensing
- `https://geodata.baltimorecity.gov/egis/rest/services/FIRE/Fire_Calls/MapServer/0/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - Fire calls, last 3 months
- `https://geodata.baltimorecity.gov/egis/rest/services/Housing/dmxLandPlanning/MapServer/21/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - Demolition permits since 2015
- `https://geodata.baltimorecity.gov/egis/rest/services/Housing/dmxBoundaries/MapServer/27/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - Tree canopy
- `https://geodata.baltimorecity.gov/egis/rest/services/Housing/dmxBoundaries/MapServer/14/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - School zones
- `https://geodata.baltimorecity.gov/egis/rest/services/CitiMap/DOT_Layers/MapServer/1/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - Bus stops
- `https://geodata.baltimorecity.gov/egis/rest/services/CitiMap/DOT_Layers/MapServer/2/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - Bus routes
- `https://geodata.baltimorecity.gov/egis/rest/services/CitiMap/DOT_Layers/MapServer/5/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - On-street bike facilities
- `https://geodata.baltimorecity.gov/egis/rest/services/Planning/Boundaries/MapServer/10/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - CHAP district
- `https://geodata.baltimorecity.gov/egis/rest/services/Planning/Boundaries/MapServer/11/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - National Register historic district
- `https://geodata.baltimorecity.gov/egis/rest/services/Housing/dmxCityPrograms/MapServer/20/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - Enterprise Zone
- `https://geodata.baltimorecity.gov/egis/rest/services/Housing/dmxBoundaries/MapServer/53/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - TOD Opportunity Zones
- `https://geodata.baltimorecity.gov/egis/rest/services/Housing/dmxBoundaries/MapServer/54/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - TOD Supportive Zones

### Baltimore context and infrastructure

- `https://data.baltimorecity.gov/api/download/v1/items/b5ee6aaebdf94241bd6700cf2eacc33b/geojson?layers=0` - Streets
- `https://data.baltimorecity.gov/api/download/v1/items/00156edf8258493fa28384e0aa8286a8/geojson?layers=0` - Major Drainage
- `https://data.baltimorecity.gov/api/download/v1/items/8ed6565f93a04455ae6bd5bcb7d272db/geojson?layers=19` - Water infrastructure layer 19
- `https://data.baltimorecity.gov/api/download/v1/items/cc307b1f087244e2bf064d211427d80e/geojson?layers=0` - CSA clogged storm drain rate
- `https://data.baltimorecity.gov/api/download/v1/items/eb55867e580740228b0d4317464ea040/geojson?layers=0` - CSA median sale price
- `https://data.baltimorecity.gov/api/download/v1/items/132ea1fbc8c84645b828a1a55406824d/geojson?layers=0` - CSA total businesses
- `https://services1.arcgis.com/mVFRs7NF4iFitgbY/arcgis/rest/services/Lifexp/FeatureServer/0/query?where=1%3D1&outFields=*&f=geojson` - Life expectancy

### Health, vulnerability, and mortgage context

- `https://services3.arcgis.com/ZvidGQkLaDJxRSJ2/arcgis/rest/services/PLACES_LocalData_for_BetterHealth/FeatureServer/3/query?where=CountyFIPS%3D%2724510%27&outFields=TractFIPS,CountyFIPS,CountyName,StateAbbr,CASTHMA_CrudePrev&f=geojson` - CDC PLACES asthma
- `https://services3.arcgis.com/ZvidGQkLaDJxRSJ2/arcgis/rest/services/PLACES_LocalData_for_BetterHealth/FeatureServer/3/query?where=CountyFIPS%3D%2724510%27&outFields=TractFIPS,CountyFIPS,CountyName,StateAbbr,DIABETES_CrudePrev&f=geojson` - CDC PLACES diabetes
- `https://services3.arcgis.com/ZvidGQkLaDJxRSJ2/arcgis/rest/services/PLACES_LocalData_for_BetterHealth/FeatureServer/3/query?where=CountyFIPS%3D%2724510%27&outFields=TractFIPS,CountyFIPS,CountyName,StateAbbr,OBESITY_CrudePrev&f=geojson` - CDC PLACES obesity
- `https://services3.arcgis.com/ZvidGQkLaDJxRSJ2/arcgis/rest/services/PLACES_LocalData_for_BetterHealth/FeatureServer/3/query?where=CountyFIPS%3D%2724510%27&outFields=TractFIPS,CountyFIPS,CountyName,StateAbbr,STROKE_CrudePrev&f=geojson` - CDC PLACES stroke
- `https://services3.arcgis.com/ZvidGQkLaDJxRSJ2/arcgis/rest/services/PLACES_LocalData_for_BetterHealth/FeatureServer/3/query?where=CountyFIPS%3D%2724510%27&outFields=TractFIPS,CountyFIPS,CountyName,StateAbbr,DEPRESSION_CrudePrev&f=geojson` - CDC PLACES depression
- `https://services3.arcgis.com/ZvidGQkLaDJxRSJ2/arcgis/rest/services/PLACES_LocalData_for_BetterHealth/FeatureServer/3/query?where=CountyFIPS%3D%2724510%27&outFields=TractFIPS,CountyFIPS,CountyName,StateAbbr,TotalPopulation,TotalPop18plus&f=geojson` - CDC PLACES total population
- `https://services3.arcgis.com/ZvidGQkLaDJxRSJ2/arcgis/rest/services/PLACES_LocalData_for_BetterHealth/FeatureServer/3/query?where=CountyFIPS%3D%2724510%27&outFields=TractFIPS,CountyFIPS,CountyName,StateAbbr,ACCESS2_CrudePrev,ACCESS2_Crude95CI&f=geojson` - CDC PLACES no health insurance
- `https://services3.arcgis.com/ZvidGQkLaDJxRSJ2/arcgis/rest/services/PLACES_LocalData_for_BetterHealth/FeatureServer/3/query?where=CountyFIPS%3D%2724510%27&outFields=TractFIPS,CountyFIPS,CountyName,StateAbbr,CHECKUP_CrudePrev,CHECKUP_Crude95CI&f=geojson` - CDC PLACES annual checkup
- `https://services3.arcgis.com/ZvidGQkLaDJxRSJ2/arcgis/rest/services/PLACES_LocalData_for_BetterHealth/FeatureServer/3/query?where=CountyFIPS%3D%2724510%27&outFields=TractFIPS,CountyFIPS,CountyName,StateAbbr,BPHIGH_CrudePrev,BPHIGH_Crude95CI&f=geojson` - CDC PLACES high blood pressure
- `https://services3.arcgis.com/ZvidGQkLaDJxRSJ2/arcgis/rest/services/PLACES_LocalData_for_BetterHealth/FeatureServer/3/query?where=CountyFIPS%3D%2724510%27&outFields=TractFIPS,CountyFIPS,CountyName,StateAbbr,CHD_CrudePrev,CHD_Crude95CI&f=geojson` - CDC PLACES coronary heart disease
- `https://services3.arcgis.com/ZvidGQkLaDJxRSJ2/arcgis/rest/services/PLACES_LocalData_for_BetterHealth/FeatureServer/3/query?where=CountyFIPS%3D%2724510%27&outFields=TractFIPS,CountyFIPS,CountyName,StateAbbr,CSMOKING_CrudePrev,CSMOKING_Crude95CI&f=geojson` - CDC PLACES current smoking
- `https://services3.arcgis.com/ZvidGQkLaDJxRSJ2/arcgis/rest/services/PLACES_LocalData_for_BetterHealth/FeatureServer/3/query?where=CountyFIPS%3D%2724510%27&outFields=TractFIPS,CountyFIPS,CountyName,StateAbbr,LPA_CrudePrev,LPA_Crude95CI&f=geojson` - CDC PLACES physical inactivity
- `https://services3.arcgis.com/ZvidGQkLaDJxRSJ2/arcgis/rest/services/PLACES_LocalData_for_BetterHealth/FeatureServer/3/query?where=CountyFIPS%3D%2724510%27&outFields=TractFIPS,CountyFIPS,CountyName,StateAbbr,MHLTH_CrudePrev,MHLTH_Crude95CI&f=geojson` - CDC PLACES frequent mental distress
- `https://onemap.cdc.gov/OneMapServices/rest/services/SVI/SVI_consolidated_data/FeatureServer/0/query?where=FIPS%20like%20%2724510______%27%20AND%20ReleaseYear%3D2022%20AND%20GeoLevel%3D%27tract%27%20AND%20Comparison%3D%27national%27&outFields=FIPS,ReleaseYear,GeoLevel,Comparison,Overall_SVI_Percentile,Theme1_Percentile,Theme2_Percentile,Theme3_Percentile,Theme4_Percentile&f=geojson` - CDC SVI 2022
- `https://ffiec.cfpb.gov/v2/data-browser-api/view/csv?counties=24510&years=2024` - HMDA 2024, all actions
- `https://ffiec.cfpb.gov/v2/data-browser-api/view/csv?counties=24510&years=2024&actions_taken=1` - HMDA 2024, originated
- `https://ffiec.cfpb.gov/v2/data-browser-api/view/csv?counties=24510&years=2024&actions_taken=3` - HMDA 2024, denied

### State and regional parcel / assessment sources

- `https://bcgisdata.baltimorecountymd.gov/arcgis/rest/services/Property/Property/MapServer/1` - Baltimore County parcel service
- `https://bcgisdata.baltimorecountymd.gov/arcgis/rest/services/Property/Property/MapServer/1/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - Baltimore County parcel query
- `https://bcgisdata.baltimorecountymd.gov/arcgis/rest/services/Property/Property/MapServer/info/iteminfo` - Baltimore County parcel metadata
- `https://data.howardcountymd.gov/` - Howard County data portal
- `https://data.howardcountymd.gov/DataDownload/ESRI/property.zip` - Howard County parcel ZIP
- `https://data.howardcountymd.gov/DataDownload/METADATA/Property.html` - Howard County parcel metadata
- `https://hcgeoserver.howardcountymd.gov:8443/geoserver/general/ows?service=WFS&version=1.0.0&request=GetFeature&typeName=general%3AProperty_Public_NoName&outputFormat=application/json` - Howard County WFS parcel feed
- `https://opendata.maryland.gov/Business-and-Economy/Maryland-Real-Property-Assessments-Hidden-Property/ed4q-f8tm` - Maryland assessment dataset page
- `https://opendata.maryland.gov/resource/ed4q-f8tm.csv` - Maryland assessment CSV
- `https://opendata.maryland.gov/api/views/ed4q-f8tm/files/5064508a-f244-4741-a548-b5ec5c5f3019?download=true&filename=Real_Property_Records_Documentation.pdf` - Maryland assessment documentation PDF

### DHCD and housing geography

- `https://mdgeodata.md.gov/imap/rest/services/BusinessEconomy/MD_MultifamilySites/FeatureServer/0` - DHCD multifamily sites
- `https://mdgeodata.md.gov/imap/rest/services/BusinessEconomy/MD_HousingDesignatedAreas/FeatureServer/0` - DHCD targeted areas
- `https://mdgeodata.md.gov/imap/rest/services/BusinessEconomy/MD_HousingDesignatedAreas/FeatureServer/1` - DHCD qualified census tracts
- `https://mdgeodata.md.gov/imap/rest/services/BusinessEconomy/MD_HousingDesignatedAreas/FeatureServer/2` - DHCD communities of opportunity
- `https://mdgeodata.md.gov/imap/rest/services/BusinessEconomy/MD_HousingDesignatedAreas/FeatureServer/3` - DHCD small difficult development areas
- `https://mdgeodata.md.gov/imap/rest/services/BusinessEconomy/MD_HousingDesignatedAreas/FeatureServer/5` - DHCD rural areas
- `https://mdgeodata.md.gov/imap/rest/services/BusinessEconomy/MD_HousingDesignatedAreas/FeatureServer/6` - DHCD BRHP opportunity designations
- `https://mdgeodata.md.gov/imap/rest/services/BusinessEconomy/MD_HousingDesignatedAreas/FeatureServer/7` - DHCD Baltimore County opportunity areas
- `https://mdgeodata.md.gov/imap/rest/services/BusinessEconomy/MD_HousingDesignatedAreas/FeatureServer/9` - DHCD just communities
- `https://services.arcgis.com/VTyQ9soqVukalItT/arcgis/rest/services/CoC_Geo_Type/FeatureServer/0/query?where=ST_1%3D%27MD%27&outFields=ST_1,STATE_NAME,COCNUM,COCNAME,HudNum,Geo_Type&returnGeometry=true&f=geojson` - HUD Continuum of Care, Maryland
- `https://services.arcgis.com/VTyQ9soqVukalItT/arcgis/rest/services/CoC_Geo_Type/FeatureServer/0/query?where=COCNUM%3D%27MD-503%27&outFields=ST_1,STATE_NAME,COCNUM,COCNAME,HudNum,Geo_Type&returnGeometry=true&f=geojson` - HUD Continuum of Care, Baltimore City
- `https://www.huduser.gov/portal/datasets/ahar.html` - HUD AHAR / PIT reference

### Extended events and capital projects

- `https://geodata.baltimorecity.gov/egis/rest/services/Planning/CIP_Equity_layers/MapServer/0/query?where=1%3D1&outFields=*&f=geojson` - FY14-20 CIP point layer
- `https://egisdata.baltimorecity.gov/egis/rest/services/Housing/DHCD_Open_Baltimore_Datasets/FeatureServer/11/query?where=1%3D1&outFields=*&f=geojson` - Foreclosure filings
- `https://egisdata.baltimorecity.gov/egis/rest/services/Housing/DHCD_Open_Baltimore_Datasets/FeatureServer/4/query?where=1%3D1&outFields=*&f=geojson` - Receivership, filed and open
- `https://egisdata.baltimorecity.gov/egis/rest/services/Housing/DHCD_Open_Baltimore_Datasets/FeatureServer/5/query?where=1%3D1&outFields=*&f=geojson` - Receivership, settled
- `https://egisdata.baltimorecity.gov/egis/rest/services/Housing/DHCD_Open_Baltimore_Datasets/FeatureServer/9/query?where=1%3D1&outFields=*&f=geojson` - Open work orders
- `https://egisdata.baltimorecity.gov/egis/rest/services/Housing/DHCD_Open_Baltimore_Datasets/FeatureServer/10/query?where=1%3D1&outFields=*&f=geojson` - Impact investment areas
- `https://data.baltimorecity.gov/api/download/v1/items/adb8e3419a2c4aad9c0c8018357ded01/geojson?layers=0` - Building permits, 2015-2018
- `https://dpwdata.baltimorecity.gov/pubgis/rest/services/Hosted/Capital_Improvement_Projects/FeatureServer/0/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - DPW CIP storm MS4
- `https://dpwdata.baltimorecity.gov/pubgis/rest/services/Hosted/Capital_Improvement_Projects/FeatureServer/1/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - DPW CIP stormwater
- `https://dpwdata.baltimorecity.gov/pubgis/rest/services/Hosted/Capital_Improvement_Projects/FeatureServer/2/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - DPW CIP water
- `https://dpwdata.baltimorecity.gov/pubgis/rest/services/Hosted/Capital_Improvement_Projects/FeatureServer/3/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - DPW CIP wastewater
- `https://dpwdata.baltimorecity.gov/pubgis/rest/services/Dashboards/Capital_Improvements/FeatureServer/0/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - Capital Improvements dashboard wastewater
- `https://dpwdata.baltimorecity.gov/pubgis/rest/services/Dashboards/Capital_Improvements/FeatureServer/1/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - Capital Improvements dashboard water
- `https://dpwdata.baltimorecity.gov/pubgis/rest/services/Dashboards/Capital_Improvements/FeatureServer/2/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - Capital Improvements dashboard stormwater
- `https://dpwdata.baltimorecity.gov/pubgis/rest/services/Dashboards/Capital_Improvements/FeatureServer/3/query?where=1%3D1&outFields=*&returnGeometry=true&f=geojson` - Capital Improvements dashboard MS4

### Capital flows and nonprofit finance

- `https://www.irs.gov/pub/irs-soi/eo_md.csv` - IRS exempt organizations, Maryland
- `https://www.irs.gov/pub/foia/ig/tege/eo-info.pdf` - IRS EO dataset guide
- `https://apps.irs.gov/pub/epostcard/990/xml/2026/2026_TEOS_XML_01A.zip` - IRS Form 990 XML part 01A
- `https://apps.irs.gov/pub/epostcard/990/xml/2026/2026_TEOS_XML_02A.zip` - IRS Form 990 XML part 02A
- `https://projects.propublica.org/nonprofits/api/` - ProPublica Nonprofit Explorer API
- `https://api.usaspending.gov/` - USAspending API
- `https://www.huduser.gov/portal/datasets/lihtc/property.html` - HUD LIHTC property database
- `https://www.ffiec.gov/data/cra/flat-files` - FFIEC CRA flat files
- `https://www.cdfifund.gov/awards/state-awards` - CDFI Fund awards

### Manual and archival references

These are tracked because they matter for research, but they are not clean operational layer downloads.

- `https://mdhistory.msa.maryland.gov/msa_brg4/brg_4_bcptaxrecs.pdf` - Baltimore City property tax records guide
- `https://msa.maryland.gov/bca/research-at-the-baltimore-city-archives/property-tax-records-for-baltimore-city/` - Baltimore City property tax records page
- `https://dat.maryland.gov/realproperty/pages/finding-your-property-information-online.aspx` - SDAT real property search
- `https://dat.maryland.gov/pages/services.aspx` - SDAT services
- `https://msa.maryland.gov/bca/research-at-the-baltimore-city-archives/the-geography-of-baltimore-city-sources/` - Baltimore geography sources
- `https://guides.loc.gov/fire-insurance-maps/sanborn-searching` - Sanborn guide
- `https://www.baltimorecitypermits.com/permit-history-request` - Permit history request flow
- `https://legislativereference.baltimorecity.gov/records-management` - Records management
- `https://apps.mht.maryland.gov/mihp/MIHP.aspx?County=Baltimore+City&Search=County` - Maryland Inventory of Historic Properties search
- `https://mdgeodata.md.gov/imap/rest/services/Historic/MD_InventoryHistoricProperties/MapServer` - Historic properties map service
- `https://www.arcgis.com/home/item.html?id=de0ddaef68624e32a84e5197c5ac1829` - 311 customer service requests 2026 dataset page
- `https://opendata.baltimorecity.gov/egis/rest/services/NonSpatialTables/Licenses/FeatureServer/0` - Baltimore liquor licenses table
- `https://health.maryland.gov/phpa/OEHFP/OFPCHS/Pages/FoodLicensePermit.aspx` - Maryland food license and permit guidance
- `https://www.courts.state.md.us/dashboards` - Maryland Courts dashboards
- `https://www.courts.state.md.us/courts/courtrecords` - Maryland court records access
- `https://pay.baltimorecity.gov/lien/announcement/index` - Baltimore lien certificate workflow
- `https://health.baltimorecity.gov/sites/default/files/Lead_Paint_VNs.pdf` - Lead paint violation notices PDF

## What This Data Is For

### Core parcel intelligence

The operational backbone is:

- Baltimore City parcel geometry
- Baltimore City real property information
- zoning
- permits
- code enforcement and vacancy signals
- tax sale / lien layers

Those are the sources most likely to affect parcel-level analysis, matching, and map filtering.

### Context layers

Health, vulnerability, life expectancy, business counts, mortgage activity, and neighborhood indicators provide tract- or CSA-scale context. They are useful for interpretation, not for parcel matching.

### Regional parcel coverage

The Maryland Planning geometry feeds are the preferred statewide starting point. Baltimore County, Howard County, and Maryland assessment sources support cross-jurisdiction work where county-native data is materially better or fills attribute gaps.

### Extended event layers

Foreclosure, receivership, work orders, impact investment areas, and CIP layers are event-style or program-style overlays. They are useful for identifying intervention patterns around parcels.

### Missing but now tracked

The repo now explicitly tracks several high-value parcel-map candidates that were previously just ideas:

- 311 requests
- rental licensing
- fire calls
- environmental citations
- demolition permits
- tree canopy
- school zones
- transit access layers
- historic district overlays
- enterprise zone and TOD policy overlays
- liquor-license, food-license, lien, eviction, and lead-enforcement research sources

### Capital flows

The capital-flows manifest is intentionally broader and less operational. Some entries are data files, some are APIs, and some are documentation endpoints for later ingestion work.

## Historical Rule

If a historical source is promoted into analytics, normalize it into parcel-event style fields:

- `blocklot` or another parcel join key
- `event_type`
- `event_date`
- `amount_usd`
- `source_name`
- `source_year`
- `confidence`

If the source does not have a direct parcel key, it should be joined spatially or by address and carry a confidence score.
